#include "internal.h"

#include "ccan/ccan/endian/endian.h"
#include "ccan/ccan/build_assert/build_assert.h"

#include <include/wally_crypto.h>
#include <include/wally_transaction.h>

#include <limits.h>
#include <stdbool.h>
#include "transaction_int.h"
#include "script_int.h"

/* Mask for the actual sighash bits */
#define SIGHASH_MASK 0x1f

/* Bytes of stack space to use to avoid allocations for tx serializing */
#define TX_STACK_SIZE 2048


#define TX_CHECK_OUTPUT if (!output) return WALLY_EINVAL; else *output = NULL
#define TX_OUTPUT_ALLOC(typ) \
    *output = wally_malloc(sizeof(typ)); \
    if (!*output) return WALLY_ENOMEM; \
    wally_clear((void *)*output, sizeof(typ)); \
    result = (typ *) *output;

/* Extra options when serialising for hashing */
struct tx_serialise_opts
{
    uint32_t sighash;                /* 8 bit sighash value for sig */
    uint32_t tx_sighash;             /* 32 bit sighash value for tx */
    size_t index;                    /* index of input we are signing */
    const unsigned char *script;     /* scriptSig of input we are signing */
    size_t script_len;               /* length of 'script' in bytes */
    uint64_t satoshi;                /* Amount of the input we are signing */
    bool bip143;                     /* Serialise for BIP143 hash */
};

static const unsigned char EMPTY_OUTPUT[9] = {
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00
};

/* LCOV_EXCL_START */
/* Check assumptions we expect to hold true */
static void assert_tx_assumptions(void)
{
    BUILD_ASSERT(WALLY_TXHASH_LEN == SHA256_LEN);
}
/* LCOV_EXCL_END */

static bool is_valid_witness_stack(const struct wally_tx_witness_stack *wit)
{
    return wit &&
           ((wit->items != NULL) == (wit->items_allocation_len != 0)) &&
           (wit->items != NULL || wit->num_items == 0);
}

static bool is_valid_tx(const struct wally_tx *tx)
{
    /* Note: The last two conditions are redundant, but having them here
     *       ensures accurate static analysis from tools like clang.
     */
    return tx &&
           ((tx->inputs != NULL) == (tx->inputs_allocation_len != 0)) &&
           ((tx->outputs != NULL) == (tx->outputs_allocation_len != 0)) &&
           (tx->num_inputs == 0 || tx->inputs != NULL) &&
           (tx->num_outputs == 0 || tx->outputs != NULL);
}

static bool is_valid_tx_input(const struct wally_tx_input *in)
{
    return in && ((in->script != NULL) == (in->script_len != 0)) &&
           (!in->witness || is_valid_witness_stack(in->witness));
}

static bool is_valid_tx_output(const struct wally_tx_output *out)
{
    return out && ((out->script != NULL) == (out->script_len != 0));
}

static void clear_and_free(void *p, size_t len)
{
    if (p) {
        wally_clear(p, len);
        wally_free(p);
    }
}

static void *realloc_array(const void *src, size_t old_n, size_t new_n, size_t size)
{
    unsigned char *p = wally_malloc(new_n * size);
    if (!p)
        return NULL;
    if (src)
        memcpy(p, src, old_n * size);
    wally_clear(p + old_n * size, (new_n - old_n) * size);
    return p;
}

static int replace_script(const unsigned char *script_in, size_t script_len_in,
                          unsigned char **script_out, size_t *script_len_out)
{
    /* TODO: Avoid reallocation if new script is smaller than the existing one */
    unsigned char *new_script = NULL;
    if (script_in) {
        if ((new_script = wally_malloc(script_len_in)) == NULL)
            return WALLY_ENOMEM;
        memcpy(new_script, script_in, script_len_in);
    }
    clear_and_free(*script_out, *script_len_out);
    *script_out = new_script;
    *script_len_out = script_len_in;
    return WALLY_OK;
}


static struct wally_tx_witness_stack *clone_witness(
    const struct wally_tx_witness_stack *wit)
{
    struct wally_tx_witness_stack *result;
    size_t i;
    int ret;

    ret = wally_tx_witness_stack_init_alloc(wit->num_items,
                                            wit->items_allocation_len, &result);

    if (ret == WALLY_OK) {
        for (i = 0; i < wit->num_items && ret == WALLY_OK; ++i) {
            if (wit->items[i].witness) {
                ret = wally_tx_witness_stack_set(result, i,
                                                 wit->items[i].witness,
                                                 wit->items[i].witness_len);
                if (ret != WALLY_OK)
                    wally_tx_witness_stack_free(result);
            }
        }
    }
    return ret == WALLY_OK ? result : NULL;
}

int wally_tx_witness_stack_init_alloc(size_t num_items,
                                      size_t allocation_len,
                                      struct wally_tx_witness_stack **output)
{
    struct wally_tx_witness_stack *result;

    if (num_items > allocation_len)
        return WALLY_EINVAL;

    TX_CHECK_OUTPUT;
    TX_OUTPUT_ALLOC(struct wally_tx_witness_stack);

    if (allocation_len) {
        result->items = wally_malloc(allocation_len * sizeof(*result->items));
        if (!result->items) {
            wally_free(result);
            *output = NULL;
            return WALLY_ENOMEM;
        }
        wally_clear(result->items, allocation_len * sizeof(*result->items));
    }
    result->items_allocation_len = allocation_len;
    result->num_items = num_items;
    return WALLY_OK;
}

static int tx_witness_stack_free(struct wally_tx_witness_stack *wit,
                                 bool free_parent)
{
    size_t i;

    if (wit) {
        if (wit->items) {
            for (i = 0; i < wit->num_items; ++i) {
                if (wit->items[i].witness)
                    clear_and_free(wit->items[i].witness,
                                   wit->items[i].witness_len);
            }
            clear_and_free(wit->items, wit->num_items * sizeof(*wit->items));
        }
        wally_clear(wit, sizeof(*wit));
        if (free_parent)
            wally_free(wit);
    }
    return WALLY_OK;
}

int wally_tx_witness_stack_free(struct wally_tx_witness_stack *wit)
{
    return tx_witness_stack_free(wit, true);
}

int wally_tx_witness_stack_add(struct wally_tx_witness_stack *wit,
                               const unsigned char *witness_in, size_t witness_len_in)
{
    if (!wit)
        return WALLY_EINVAL;
    return wally_tx_witness_stack_set(wit, wit->num_items,
                                      witness_in, witness_len_in);
}

int wally_tx_witness_stack_set(struct wally_tx_witness_stack *wit, size_t index,
                               const unsigned char *witness_in, size_t witness_len_in)
{
    unsigned char *new_witness = NULL;

    if (!is_valid_witness_stack(wit) || (!witness_in && witness_len_in))
        return WALLY_EINVAL;

    if (witness_len_in) {
        new_witness = wally_malloc(witness_len_in);
        if (!new_witness)
            return WALLY_ENOMEM;
    }

    if (index >= wit->num_items) {
        if (index >= wit->items_allocation_len) {
            /* Expand the witness array */
            struct wally_tx_witness_item *p;
            p = realloc_array(wit->items, wit->items_allocation_len,
                              index + 1, sizeof(*wit->items));
            if (!p) {
                wally_free(new_witness);
                return WALLY_ENOMEM;
            }
            clear_and_free(wit->items, wit->num_items * sizeof(*wit->items));
            wit->items = p;
            wit->items_allocation_len = index + 1;
        }
        wit->num_items = index + 1;
    }
    clear_and_free(wit->items[index].witness, wit->items[index].witness_len);
    if (witness_len_in)
        memcpy(new_witness, witness_in, witness_len_in);
    wit->items[index].witness = new_witness;
    wit->items[index].witness_len = witness_len_in;
    return WALLY_OK;
}

static bool clone_input_to(
    struct wally_tx_input *dst,
    const struct wally_tx_input *src)
{
    unsigned char *new_script = wally_malloc(src->script_len);
    struct wally_tx_witness_stack *new_witness;
    new_witness = new_script && src->witness ? clone_witness(src->witness) : NULL;

    if (!new_script || (src->witness && !new_witness)) {
        wally_free(new_script);
        return false;
    }

    memcpy(dst, src, sizeof(*src));
    memcpy(new_script, src->script, src->script_len);
    dst->script = new_script;
    dst->witness = new_witness;
    return true;
}

int wally_tx_input_init(const unsigned char *txhash_in, size_t txhash_len_in,
                        uint32_t index, uint32_t sequence,
                        const unsigned char *script_in, size_t script_len_in,
                        const struct wally_tx_witness_stack *witness_in,
                        struct wally_tx_input *output)
{
    if (!txhash_in || txhash_len_in != WALLY_TXHASH_LEN ||
        ((script_in != NULL) != (script_len_in != 0)) || !output)
        return WALLY_EINVAL;

    if (!script_in)
        output->script = NULL;
    else if (!(output->script = wally_malloc(script_len_in)))
        return WALLY_ENOMEM;

    output->witness = NULL;
    if (witness_in && !(output->witness = clone_witness(witness_in))) {
        wally_free(output->script);
        output->script = NULL;
        return WALLY_ENOMEM;
    }

    output->index = index;
    output->sequence = sequence;
    memcpy(output->txhash, txhash_in, WALLY_TXHASH_LEN);
    if (output->script)
        memcpy(output->script, script_in, script_len_in);
    output->script_len = script_len_in;
    return WALLY_OK;
}

int wally_tx_input_init_alloc(const unsigned char *txhash_in, size_t txhash_len_in,
                              uint32_t index, uint32_t sequence,
                              const unsigned char *script_in, size_t script_len_in,
                              const struct wally_tx_witness_stack *witness_in,
                              struct wally_tx_input **output)
{
    struct wally_tx_input *result;
    int ret;

    TX_CHECK_OUTPUT;
    TX_OUTPUT_ALLOC(struct wally_tx_input);

    ret = wally_tx_input_init(txhash_in, txhash_len_in, index, sequence,
                              script_in, script_len_in, witness_in, result);

    if (ret != WALLY_OK) {
        clear_and_free(result, sizeof(*result));
        *output = NULL;
    }
    return ret;
}

static int tx_input_free(struct wally_tx_input *in, bool free_parent)
{
    if (in) {
        clear_and_free(in->script, in->script_len);
        tx_witness_stack_free(in->witness, true);
        wally_clear(in, sizeof(*in));
        if (free_parent)
            wally_free(in);
    }
    return WALLY_OK;
}

int wally_tx_input_free(struct wally_tx_input *in)
{
    return tx_input_free(in, true);
}

static bool clone_output_to(struct wally_tx_output *dst,
                            const struct wally_tx_output *src)
{
    unsigned char *new_script = wally_malloc(src->script_len);
    if (!new_script)
        return false;

    dst->satoshi = src->satoshi;
    memcpy(new_script, src->script, src->script_len);
    dst->script = new_script;
    dst->script_len = src->script_len;
    return true;
}

int wally_tx_output_init(uint64_t satoshi,
                         const unsigned char *script_in, size_t script_len_in,
                         struct wally_tx_output *output)
{
    if (((script_in != NULL) != (script_len_in != 0)) || !output)
        return WALLY_EINVAL;

    if (!script_in)
        output->script = NULL;
    else if (!(output->script = wally_malloc(script_len_in)))
        return WALLY_ENOMEM;

    if (output->script)
        memcpy(output->script, script_in, script_len_in);
    output->script_len = script_len_in;
    output->satoshi = satoshi;
    return WALLY_OK;
}

int wally_tx_output_init_alloc(uint64_t satoshi,
                               const unsigned char *script_in, size_t script_len_in,
                               struct wally_tx_output **output)
{
    struct wally_tx_output *result;
    int ret;

    TX_CHECK_OUTPUT;
    TX_OUTPUT_ALLOC(struct wally_tx_output);

    ret = wally_tx_output_init(satoshi, script_in, script_len_in, result);

    if (ret != WALLY_OK) {
        clear_and_free(result, sizeof(*result));
        *output = NULL;
    }
    return ret;
}

static int tx_output_free(struct wally_tx_output *output, bool free_parent)
{
    if (output) {
        clear_and_free(output->script, output->script_len);
        wally_clear(output, sizeof(*output));
        if (free_parent)
            wally_free(output);
    }
    return WALLY_OK;
}

int wally_tx_output_free(struct wally_tx_output *output)
{
    return tx_output_free(output, true);
}

int wally_tx_init_alloc(uint32_t version, uint32_t locktime,
                        size_t inputs_allocation_len,
                        size_t outputs_allocation_len,
                        struct wally_tx **output)
{
    struct wally_tx_input *new_inputs = NULL;
    struct wally_tx_output *new_outputs = NULL;
    struct wally_tx *result;

    TX_CHECK_OUTPUT;
    TX_OUTPUT_ALLOC(struct wally_tx);

    if (inputs_allocation_len)
        new_inputs = wally_malloc(inputs_allocation_len * sizeof(struct wally_tx_input));
    if (outputs_allocation_len)
        new_outputs = wally_malloc(outputs_allocation_len * sizeof(struct wally_tx_output));
    if ((inputs_allocation_len && !new_inputs) ||
        (outputs_allocation_len && !new_outputs)) {
        wally_free(new_inputs);
        wally_free(new_outputs);
        wally_free(result);
        *output = NULL;
        return WALLY_ENOMEM;
    }

    result->version = version;
    result->locktime = locktime;
    result->inputs = new_inputs;
    result->num_inputs = 0;
    result->inputs_allocation_len = inputs_allocation_len;
    result->outputs = new_outputs;
    result->num_outputs = 0;
    result->outputs_allocation_len = outputs_allocation_len;
    return WALLY_OK;
}

static int tx_free(struct wally_tx *tx, bool free_parent)
{
    size_t i;
    if (tx) {
        for (i = 0; i < tx->num_inputs; ++i)
            tx_input_free(&tx->inputs[i], false);
        clear_and_free(tx->inputs, tx->inputs_allocation_len * sizeof(*tx->inputs));
        for (i = 0; i < tx->num_outputs; ++i)
            tx_output_free(&tx->outputs[i], false);
        clear_and_free(tx->outputs, tx->outputs_allocation_len * sizeof(*tx->outputs));
        wally_clear(tx, sizeof(*tx));
        if (free_parent)
            wally_free(tx);
    }
    return WALLY_OK;
}

int wally_tx_free(struct wally_tx *tx)
{
    return tx_free(tx, true);
}

int wally_tx_add_input(struct wally_tx *tx, const struct wally_tx_input *input)
{
    if (!is_valid_tx(tx) || !is_valid_tx_input(input))
        return WALLY_EINVAL;

    if (tx->num_inputs >= tx->inputs_allocation_len) {
        /* Expand the inputs array */
        struct wally_tx_input *p;
        p = realloc_array(tx->inputs, tx->inputs_allocation_len,
                          tx->num_inputs + 1, sizeof(*tx->inputs));
        if (!p)
            return WALLY_ENOMEM;

        clear_and_free(tx->inputs, tx->num_inputs * sizeof(*tx->inputs));
        tx->inputs = p;
        tx->inputs_allocation_len += 1;
    }
    if (!clone_input_to(tx->inputs + tx->num_inputs, input))
        return WALLY_ENOMEM;

    tx->num_inputs += 1;
    return WALLY_OK;
}

int wally_tx_remove_input(struct wally_tx *tx, size_t index)
{
    struct wally_tx_input *input;

    if (!is_valid_tx(tx) || index >= tx->num_inputs)
        return WALLY_EINVAL;

    input = tx->inputs + index;
    tx_input_free(input, false);
    if (index != tx->num_inputs - 1)
        memmove(input, input + 1,
                (tx->num_inputs - index - 1) * sizeof(*input));
    wally_clear(tx->inputs + tx->num_inputs - 1, sizeof(*input));

    tx->num_inputs -= 1;
    return WALLY_OK;
}

int wally_tx_add_output(struct wally_tx *tx, const struct wally_tx_output *output)
{
    if (!is_valid_tx(tx) || !is_valid_tx_output(output))
        return WALLY_EINVAL;

    if (tx->num_outputs >= tx->outputs_allocation_len) {
        /* Expand the outputs array */
        struct wally_tx_output *p;
        p = realloc_array(tx->outputs, tx->outputs_allocation_len,
                          tx->num_outputs + 1, sizeof(*tx->outputs));
        if (!p)
            return WALLY_ENOMEM;

        clear_and_free(tx->outputs, tx->num_outputs * sizeof(*tx->outputs));
        tx->outputs = p;
        tx->outputs_allocation_len += 1;
    }
    if (!clone_output_to(tx->outputs + tx->num_outputs, output))
        return WALLY_ENOMEM;

    tx->num_outputs += 1;
    return WALLY_OK;
}

int wally_tx_remove_output(struct wally_tx *tx, size_t index)
{
    struct wally_tx_output *output;

    if (!is_valid_tx(tx) || index >= tx->num_outputs)
        return WALLY_EINVAL;

    output = tx->outputs + index;
    tx_output_free(output, false);
    if (index != tx->num_outputs - 1)
        memmove(output, output + 1,
                (tx->num_outputs - index - 1) * sizeof(*output));
    wally_clear(tx->outputs + tx->num_outputs - 1, sizeof(*output));

    tx->num_outputs -= 1;
    return WALLY_OK;
}

int wally_tx_get_witness_count(const struct wally_tx *tx, size_t *written)
{
    size_t i;

    if (written)
        *written = 0;

    if (!is_valid_tx(tx) || !written)
        return WALLY_EINVAL;

    for (i = 0; i < tx->num_inputs; ++i)
        if (tx->inputs[i].witness)
            *written += 1;

    return WALLY_OK;
}

/* We compute the size of the witness separately so we can compute vsize
 * without iterating the transaction twice with different flags.
 */
static int tx_get_lengths(const struct wally_tx *tx,
                          const struct tx_serialise_opts *opts, uint32_t flags,
                          size_t *base_size, size_t *witness_size,
                          size_t *witness_count)
{
    size_t n, i, j;
    const bool anyonecanpay = opts && opts->sighash & WALLY_SIGHASH_ANYONECANPAY;
    const bool sh_none = opts && (opts->sighash & SIGHASH_MASK) == WALLY_SIGHASH_NONE;
    const bool sh_single = opts && (opts->sighash & SIGHASH_MASK) == WALLY_SIGHASH_SINGLE;

    *witness_count = 0;

    if (opts) {
        if (flags & WALLY_TX_FLAG_USE_WITNESS)
            return WALLY_ERROR; /* Segwit tx hashing uses bip143 opts member */

        if (opts->bip143) {
            *base_size = sizeof(uint32_t) + /* version */
                         SHA256_LEN + /* hash prevouts */
                         SHA256_LEN + /* hash sequence */
                         WALLY_TXHASH_LEN + sizeof(uint32_t) + /* outpoint + index */
                         varbuff_get_length(opts->script_len) + /* script */
                         sizeof(uint64_t) + /* amount */
                         sizeof(uint32_t) + /* input sequence */
                         SHA256_LEN + /* hash outputs */
                         sizeof(uint32_t) + /* nlocktime */
                         sizeof(uint32_t); /* tx sighash */
            *witness_size = 0;
            return WALLY_OK;
        }
    }

    if ((flags & ~WALLY_TX_FLAG_USE_WITNESS) ||
        ((flags & WALLY_TX_FLAG_USE_WITNESS) &&
         wally_tx_get_witness_count(tx, witness_count) != WALLY_OK))
        return WALLY_EINVAL;

    if (!*witness_count)
        flags &= ~WALLY_TX_FLAG_USE_WITNESS;

    n = sizeof(tx->version) +
        varint_get_length(anyonecanpay ? 1 : tx->num_inputs) +
        (sh_none ? 1 : varint_get_length(sh_single ? opts->index + 1 : tx->num_outputs)) +
        sizeof(tx->locktime) +
        (opts ? sizeof(leint32_t) : 0); /* Include trailing tx_sighash */

    for (i = 0; i < tx->num_inputs; ++i) {
        const struct wally_tx_input *input = tx->inputs + i;
        if (anyonecanpay && i != opts->index)
            continue; /* anyonecanpay only signs the given index */

        n += sizeof(input->txhash) +
             sizeof(input->index) +
             sizeof(input->sequence);

        if (opts) {
            if (i == opts->index)
                n += varbuff_get_length(opts->script_len);
            else
                ++n;
        } else
            n += varbuff_get_length(input->script_len);

    }

    if (!sh_none) {
        size_t num_outputs = sh_single ? opts->index + 1 : tx->num_outputs;

        for (i = 0; i < num_outputs; ++i) {
            const struct wally_tx_output *output = tx->outputs + i;
            if (sh_single && i != opts->index)
                n += sizeof(EMPTY_OUTPUT);
            else
                n += sizeof(output->satoshi) +
                     varbuff_get_length(output->script_len);
        }
    }

    *base_size = n;

    n = 0;
    if (flags & WALLY_TX_FLAG_USE_WITNESS) {
        n = 2; /* For marker and flag bytes 0x00 0x01 */

        for (i = 0; i < tx->num_inputs; ++i) {
            const struct wally_tx_input *input = tx->inputs + i;
            size_t num_items = input->witness ? input->witness->num_items : 0;
            n += varint_get_length(num_items);
            for (j = 0; j < num_items; ++j) {
                const struct wally_tx_witness_item *wit;
                wit = input->witness->items + j;
                n += varbuff_get_length(wit->witness_len);
            }
        }
    }

    *witness_size = n;
    return WALLY_OK;
}

static int tx_get_length(const struct wally_tx *tx,
                         const struct tx_serialise_opts *opts, uint32_t flags,
                         size_t *written)
{
    size_t base_size, witness_size, witness_count;

    if (written)
        *written = 0;

    if (!written ||
        tx_get_lengths(tx, opts, flags, &base_size, &witness_size,
                       &witness_count) != WALLY_OK)
        return WALLY_EINVAL;

    if (witness_count && (flags & WALLY_TX_FLAG_USE_WITNESS))
        *written = base_size + witness_size;
    else
        *written = base_size;

    return WALLY_OK;
}

int wally_tx_get_length(const struct wally_tx *tx, uint32_t flags,
                        size_t *written)
{
    return tx_get_length(tx, NULL, flags, written);
}

int wally_tx_get_weight(const struct wally_tx *tx, size_t *written)
{
    size_t base_size, witness_size, witness_count;

    if (written)
        *written = 0;

    if (!written ||
        tx_get_lengths(tx, NULL, WALLY_TX_FLAG_USE_WITNESS, &base_size,
                       &witness_size, &witness_count) != WALLY_OK)
        return WALLY_EINVAL;

    if (witness_count)
        *written = base_size * 4 + witness_size;
    else
        *written = base_size * 4;

    return WALLY_OK;
}

int wally_tx_vsize_from_weight(size_t weight, size_t *written)
{
    *written = (weight + 3) / 4; /* ceil(weight/4) */
    return WALLY_OK;
}

int wally_tx_get_vsize(const struct wally_tx *tx, size_t *written)
{
    int ret = wally_tx_get_weight(tx, written);
    if (ret == WALLY_OK)
        ret = wally_tx_vsize_from_weight(*written, written);
    return ret;
}

static inline int tx_to_bip143_bytes(const struct wally_tx *tx,
                                     const struct tx_serialise_opts *opts,
                                     uint32_t flags,
                                     unsigned char *bytes_out, size_t len,
                                     size_t *written)
{
    unsigned char buff[TX_STACK_SIZE / 2], *buff_p = buff;
    size_t i, inputs_size, outputs_size, buff_len = sizeof(buff);
    const bool anyonecanpay = opts->sighash & WALLY_SIGHASH_ANYONECANPAY;
    const bool sh_none = (opts->sighash & SIGHASH_MASK) == WALLY_SIGHASH_NONE;
    const bool sh_single = (opts->sighash & SIGHASH_MASK) == WALLY_SIGHASH_SINGLE;
    unsigned char *p = bytes_out, *output_p;
    int ret = WALLY_OK;

    (void)flags;
    (void)len;

    /* Note we assume tx_to_bytes has already validated all inputs */
    p += uint32_to_le_bytes(tx->version, p);

    inputs_size = tx->num_inputs * (WALLY_TXHASH_LEN + sizeof(uint32_t));
    if (sh_none || (sh_single && opts->index >= tx->num_outputs))
        outputs_size = 0;
    else if (sh_single)
        outputs_size = sizeof(uint64_t) +
                       varbuff_get_length(tx->outputs[opts->index].script_len);
    else {
        outputs_size = 0;
        for (i = 0; i < tx->num_outputs; ++i) {
            outputs_size += sizeof(uint64_t) +
                            varbuff_get_length(tx->outputs[i].script_len);
        }
    }

    if (inputs_size > buff_len || outputs_size > buff_len) {
        buff_len = inputs_size > outputs_size ? inputs_size : outputs_size;
        buff_p = wally_malloc(buff_len);
        if (buff_p == NULL)
            return WALLY_ENOMEM;
    }

    /* Inputs */
    if (anyonecanpay)
        memset(p, 0, SHA256_LEN);
    else {
        for (i = 0; i < tx->num_inputs; ++i) {
            unsigned char *tmp_p = buff_p + i * (WALLY_TXHASH_LEN + sizeof(uint32_t));
            memcpy(tmp_p, tx->inputs[i].txhash, WALLY_TXHASH_LEN);
            uint32_to_le_bytes(tx->inputs[i].index, tmp_p + WALLY_TXHASH_LEN);
        }

        if ((ret = wally_sha256d(buff_p, inputs_size, p, SHA256_LEN)) != WALLY_OK)
            goto error;
    }
    p += SHA256_LEN;

    /* Sequences */
    if (anyonecanpay || sh_single || sh_none)
        memset(p, 0, SHA256_LEN);
    else {
        for (i = 0; i < tx->num_inputs; ++i)
            uint32_to_le_bytes(tx->inputs[i].sequence, buff_p + i * sizeof(uint32_t));

        ret = wally_sha256d(buff_p, tx->num_inputs * sizeof(uint32_t), p, SHA256_LEN);
        if (ret != WALLY_OK)
            goto error;
    }
    p += SHA256_LEN;

    /* Input details */
    memcpy(p, tx->inputs[opts->index].txhash, WALLY_TXHASH_LEN);
    p += WALLY_TXHASH_LEN;
    p += uint32_to_le_bytes(tx->inputs[opts->index].index, p);
    p += varbuff_to_bytes(opts->script, opts->script_len, p);
    p += uint64_to_le_bytes(opts->satoshi, p);
    p += uint32_to_le_bytes(tx->inputs[opts->index].sequence, p);

    /* Outputs */
    if (sh_none || (sh_single && opts->index >= tx->num_outputs))
        memset(p, 0, SHA256_LEN);
    else {
        output_p = buff_p;
        for (i = 0; i < tx->num_outputs; ++i) {
            if (sh_single && i != opts->index)
                continue;
            output_p += uint64_to_le_bytes(tx->outputs[i].satoshi, output_p);
            output_p += varbuff_to_bytes(tx->outputs[i].script,
                                         tx->outputs[i].script_len, output_p);
        }

        ret = wally_sha256d(buff_p, outputs_size, p, SHA256_LEN);
        if (ret != WALLY_OK)
            goto error;
    }
    p += SHA256_LEN;

    /* nlocktime and sighash*/
    p += uint32_to_le_bytes(tx->locktime, p);
    p += uint32_to_le_bytes(opts->tx_sighash, p);

    *written = p - bytes_out;

error:
    if (buff_p != buff)
        clear_and_free(buff_p, buff_len);
    else
        wally_clear(buff, sizeof(buff));
    return ret;
}

static int tx_to_bytes(const struct wally_tx *tx,
                       const struct tx_serialise_opts *opts,
                       uint32_t flags,
                       unsigned char *bytes_out, size_t len,
                       size_t *written)
{
    size_t n, i, j, witness_count;
    const bool anyonecanpay = opts && opts->sighash & WALLY_SIGHASH_ANYONECANPAY;
    const bool sh_none = opts && (opts->sighash & SIGHASH_MASK) == WALLY_SIGHASH_NONE;
    const bool sh_single = opts && (opts->sighash & SIGHASH_MASK) == WALLY_SIGHASH_SINGLE;
    unsigned char *p = bytes_out;

    if (written)
        *written = 0;

    if (!is_valid_tx(tx) || (flags & ~WALLY_TX_FLAG_USE_WITNESS) ||
        !bytes_out || !written ||
        tx_get_length(tx, opts, flags, &n) != WALLY_OK)
        return WALLY_EINVAL;

    if (opts && (flags & WALLY_TX_FLAG_USE_WITNESS))
        return WALLY_ERROR; /* Segwit tx hashing is handled elsewhere */

    if (n > len) {
        *written = n;
        return WALLY_OK;
    }

    if (opts && opts->bip143)
        return tx_to_bip143_bytes(tx, opts, flags, bytes_out, len, written);

    if (flags & WALLY_TX_FLAG_USE_WITNESS) {
        if (wally_tx_get_witness_count(tx, &witness_count) != WALLY_OK)
            return WALLY_EINVAL;
        if (!witness_count)
            flags &= ~WALLY_TX_FLAG_USE_WITNESS;
    }

    p += uint32_to_le_bytes(tx->version, p);
    if (flags & WALLY_TX_FLAG_USE_WITNESS) {
        *p++ = 0; /* Write BIP 144 marker */
        *p++ = 1; /* Write BIP 144 flag */
    }
    if (anyonecanpay)
        *p++ = 1;
    else
        p += varint_to_bytes(tx->num_inputs, p);

    for (i = 0; i < tx->num_inputs; ++i) {
        const struct wally_tx_input *input = tx->inputs + i;
        if (anyonecanpay && i != opts->index)
            continue; /* anyonecanpay only signs the given index */

        memcpy(p, input->txhash, sizeof(input->txhash));
        p += sizeof(input->txhash);
        p += uint32_to_le_bytes(input->index, p);
        if (opts) {
            if (i == opts->index)
                p += varbuff_to_bytes(opts->script, opts->script_len, p);
            else
                *p++ = 0; /* Blank scripts for non-signing inputs */
        } else
            p += varbuff_to_bytes(input->script, input->script_len, p);

        if ((sh_none || sh_single) && i != opts->index)
            p += uint32_to_le_bytes(0, p);
        else
            p += uint32_to_le_bytes(input->sequence, p);
    }

    if (sh_none)
        *p++ = 0;
    else {
        size_t num_outputs = sh_single ? opts->index + 1 : tx->num_outputs;
        p += varint_to_bytes(num_outputs, p);

        for (i = 0; i < num_outputs; ++i) {
            const struct wally_tx_output *output = tx->outputs + i;
            if (sh_single && i != opts->index) {
                memcpy(p, EMPTY_OUTPUT, sizeof(EMPTY_OUTPUT));
                p += sizeof(EMPTY_OUTPUT);
            } else {
                p += uint64_to_le_bytes(output->satoshi, p);
                p += varbuff_to_bytes(output->script, output->script_len, p);
            }
        }
    }

    if (flags & WALLY_TX_FLAG_USE_WITNESS) {
        for (i = 0; i < tx->num_inputs; ++i) {
            const struct wally_tx_input *input = tx->inputs + i;
            size_t num_items = input->witness ? input->witness->num_items : 0;
            p += varint_to_bytes(num_items, p);
            for (j = 0; j < num_items; ++j) {
                const struct wally_tx_witness_item *wit;
                wit = input->witness->items + j;
                p += varbuff_to_bytes(wit->witness, wit->witness_len, p);
            }
        }
    }

    p += uint32_to_le_bytes(tx->locktime, p);
    if (opts)
        uint32_to_le_bytes(opts->tx_sighash, p);
    *written = n;
    return WALLY_OK;
}

int wally_tx_to_bytes(const struct wally_tx *tx, uint32_t flags,
                      unsigned char *bytes_out, size_t len,
                      size_t *written)
{
    return tx_to_bytes(tx, NULL, flags, bytes_out, len, written);
}

int wally_tx_to_hex(const struct wally_tx *tx, uint32_t flags,
                    char **output)
{
    unsigned char buff[TX_STACK_SIZE], *buff_p = buff;
    size_t n, written;
    int ret;

    if (!output)
        return WALLY_EINVAL;

    ret = wally_tx_to_bytes(tx, flags, buff_p, sizeof(buff), &n);
    if (ret == WALLY_OK) {
        if (n > sizeof(buff)) {
            if ((buff_p = wally_malloc(n)) == NULL)
                return WALLY_ENOMEM;
            ret = wally_tx_to_bytes(tx, flags, buff_p, n, &written);
            if (n != written)
                ret = WALLY_ERROR; /* Length calculated incorrectly */
        }
        if (ret == WALLY_OK)
            ret = wally_hex_from_bytes(buff_p, n, output);
        if (buff_p != buff)
            clear_and_free(buff_p, n);
        else
            wally_clear(buff, n);
    }
    return ret;
}

static int analyze_tx(const unsigned char *bytes_in, size_t len_in,
                      uint32_t flags, size_t *num_inputs, size_t *num_outputs,
                      bool *expect_witnesses)
{
    const unsigned char *p = bytes_in, *end = bytes_in + len_in;
    uint64_t v;
    size_t i, j, num_witnesses;
    struct wally_tx tmp_tx;

    if (num_inputs)
        *num_inputs = 0;
    if (num_outputs)
        *num_outputs = 0;
    if (expect_witnesses)
        *expect_witnesses = false;

    if (!bytes_in || len_in < sizeof(uint32_t) + 2 || flags ||
        !num_inputs || !num_outputs || !expect_witnesses)
        return WALLY_EINVAL;

    *expect_witnesses = false;

    p += uint32_from_le_bytes(p, &tmp_tx.version);
    if (!tmp_tx.version || tmp_tx.version > WALLY_TX_MAX_VERSION)
        return WALLY_EINVAL;

    if (*p == 0) {
        /* BIP 144 extended serialization */
        if (p[1] != 0x1)
            return WALLY_EINVAL; /* Invalid witness flag */
        p += 2;
        *expect_witnesses = true;
    }

#define ensure_n(n) if (p > end || p + (n) > end) return WALLY_EINVAL

#define ensure_varint(dst) ensure_n(varint_length_from_bytes(p)); \
    p += varint_from_bytes(p, (dst))

#define ensure_varbuff(dst) ensure_varint((dst)); \
    ensure_n(*dst)

    ensure_varint(&v);
    if (!v)
        return WALLY_EINVAL;
    *num_inputs = v;

    for (i = 0; i < *num_inputs; ++i) {
        ensure_n(WALLY_TXHASH_LEN + sizeof(uint32_t));
        p += WALLY_TXHASH_LEN + sizeof(uint32_t);
        ensure_varbuff(&v);
        /* FIXME: Analyse script types if required */
        p += v;
        ensure_n(sizeof(uint32_t));
        p += sizeof(uint32_t);
    }

    ensure_varint(&v);
    if (!v)
        return WALLY_EINVAL;
    *num_outputs = v;

    for (i = 0; i < *num_outputs; ++i) {
        ensure_n(sizeof(uint64_t));
        p += sizeof(uint64_t);
        ensure_varbuff(&v);
        /* FIXME: Analyse script types if required */
        p += v;
    }

    if (*expect_witnesses) {
        for (i = 0; i < *num_inputs; ++i) {
            ensure_varint(&num_witnesses);
            for (j = 0; j < num_witnesses; ++j) {
                ensure_varbuff(&v);
                p += v;
            }
        }
    }

    ensure_n(sizeof(uint32_t)); /* Locktime */

#undef ensure_n
#undef ensure_varint
#undef ensure_varbuff
    return WALLY_OK;
}

int wally_tx_from_bytes(const unsigned char *bytes_in, size_t len_in,
                        uint32_t flags, struct wally_tx **output)
{
    const unsigned char *p = bytes_in;
    bool expect_witnesses;
    uint32_t analyze_flags = flags & ~WALLY_TX_FLAG_USE_WITNESS;
    size_t i, j, num_inputs, num_outputs, num_witnesses;
    int ret;
    struct wally_tx *result;

    TX_CHECK_OUTPUT;

    if (analyze_tx(bytes_in, len_in, analyze_flags, &num_inputs, &num_outputs,
                   &expect_witnesses) != WALLY_OK)
        return WALLY_EINVAL;

    ret = wally_tx_init_alloc(0, 0, num_inputs, num_outputs, output);
    if (ret != WALLY_OK)
        return ret;
    result = (struct wally_tx *)*output;

    p += uint32_from_le_bytes(p, &result->version);
    if (expect_witnesses)
        p += 2; /* Skip flag bytes */
    p += varint_from_bytes(p, &num_inputs);

    for (i = 0; i < num_inputs; ++i) {
        const unsigned char *txhash = p, *script;
        uint32_t index, sequence;
        size_t script_len;
        p += WALLY_TXHASH_LEN;
        p += uint32_from_le_bytes(p, &index);
        p += varint_from_bytes(p, &script_len);
        script = p;
        p += script_len;
        p += uint32_from_le_bytes(p, &sequence);
        ret = wally_tx_input_init(txhash, WALLY_TXHASH_LEN, index, sequence,
                                  script_len ? script : NULL, script_len, NULL,
                                  &result->inputs[i]);
        if (ret != WALLY_OK)
            goto fail;
        result->num_inputs += 1;
    }

    p += varint_from_bytes(p, &num_outputs);
    for (i = 0; i < num_outputs; ++i) {
        const unsigned char *script;
        uint64_t satoshi;
        size_t script_len;
        p += uint64_from_le_bytes(p, &satoshi);
        p += varint_from_bytes(p, &script_len);
        script = p;
        p += script_len;
        ret = wally_tx_output_init(satoshi, script, script_len,
                                   &result->outputs[i]);
        if (ret != WALLY_OK)
            goto fail;
        result->num_outputs += 1;
    }

    if (expect_witnesses) {
        for (i = 0; i < num_inputs; ++i) {
            p += varint_from_bytes(p, &num_witnesses);
            if (!num_witnesses)
                continue;
            ret = wally_tx_witness_stack_init_alloc(num_witnesses, num_witnesses,
                                                    &result->inputs[i].witness);
            if (ret != WALLY_OK)
                goto fail;

            for (j = 0; j < num_witnesses; ++j) {
                size_t witness_len;
                p += varint_from_bytes(p, &witness_len);
                ret = wally_tx_witness_stack_set(result->inputs[i].witness, j,
                                                 p, witness_len);
                if (ret != WALLY_OK)
                    goto fail;
                p += witness_len;
            }
        }
    }

    uint32_from_le_bytes(p, &result->locktime);
    return WALLY_OK;
fail:
    tx_free(result, true);
    *output = NULL;
    return ret;
}

int wally_tx_from_hex(const char *hex, uint32_t flags,
                      struct wally_tx **output)
{
    unsigned char buff[TX_STACK_SIZE], *buff_p = buff;
    size_t hex_len = hex ? strlen(hex) : 0, bin_len;
    size_t written;
    int ret;

    if (!hex || hex_len & 0x1 || !output)
        return WALLY_EINVAL;

    bin_len = hex_len / 2;

    if (bin_len > sizeof(buff)) {
        if ((buff_p = wally_malloc(bin_len)) == NULL)
            return WALLY_ENOMEM;
    }
    ret = wally_hex_to_bytes(hex, buff_p, bin_len, &written);
    if (ret == WALLY_OK)
        ret = wally_tx_from_bytes(buff_p, bin_len, flags, output);

    if (buff_p != buff)
        clear_and_free(buff_p, bin_len);
    else
        wally_clear(buff, bin_len);

    return ret;
}

int wally_tx_get_signature_hash(const struct wally_tx *tx,
                                size_t index,
                                const unsigned char *script_in, size_t script_len_in,
                                const unsigned char *extra_in, size_t extra_len_in,
                                uint32_t extra_offset, uint64_t satoshi,
                                uint32_t sighash, uint32_t tx_sighash, uint32_t flags,
                                unsigned char *bytes_out, size_t len)
{
    unsigned char buff[TX_STACK_SIZE], *buff_p = buff;
    size_t n, n2;
    int ret;
    const struct tx_serialise_opts opts = {
        sighash, tx_sighash, index, script_in, script_len_in, satoshi,
        (flags & WALLY_TX_FLAG_USE_WITNESS) ? true : false
    };

    if (!is_valid_tx(tx) || ((script_in != NULL) != (script_len_in != 0)) ||
        (flags & ~WALLY_TX_FLAG_USE_WITNESS) || !bytes_out || len < SHA256_LEN)
        return WALLY_EINVAL;

    if (extra_in || extra_len_in || extra_offset)
        return WALLY_ERROR; /* FIXME: Not implemented yet */

    if (index >= tx->num_inputs ||
        (index >= tx->num_outputs && (sighash & SIGHASH_MASK) == WALLY_SIGHASH_SINGLE)) {
        if (!(flags & WALLY_TX_FLAG_USE_WITNESS)) {
            memset(bytes_out, 0, SHA256_LEN);
            bytes_out[0] = 0x1;
            return WALLY_OK;
        }
    }

    if ((ret = tx_get_length(tx, &opts, 0, &n)) != WALLY_OK)
        goto fail;

    if (n > sizeof(buff) && (buff_p = wally_malloc(n)) == NULL) {
        ret = WALLY_ENOMEM;
        goto fail;
    }

    if ((ret = tx_to_bytes(tx, &opts, 0, buff_p, n, &n2)) != WALLY_OK)
        goto fail;

    if (n != n2)
        ret = WALLY_ERROR; /* tx_get_length/tx_to_bytes mismatch, should not happen! */
    else
        ret = wally_sha256d(buff_p, n2, bytes_out, len);

fail:
    if (buff_p != buff)
        clear_and_free(buff_p, n);
    else
        wally_clear(buff, sizeof(buff));
    return ret;
}

int wally_tx_get_btc_signature_hash(const struct wally_tx *tx, size_t index,
                                    const unsigned char *script_in, size_t script_len_in,
                                    uint64_t satoshi, uint32_t sighash, uint32_t flags,
                                    unsigned char *bytes_out, size_t len)
{
    return wally_tx_get_signature_hash(tx, index, script_in, script_len_in,
                                       NULL, 0, 0, satoshi, sighash, sighash,
                                       flags, bytes_out, len);
}

#if defined (SWIG_JAVA_BUILD) || defined (SWIG_PYTHON_BUILD) || defined (SWIG_JAVASCRIPT_BUILD)

/* Getters for wally_tx_input/wally_tx_output/wally_tx values */

static int tx_getb_impl(const void *in,
                        const unsigned char *src, size_t src_len,
                        unsigned char *bytes_out, size_t len, size_t *written)
{
    if (written)
        *written = 0;
    if (!in || !bytes_out || len < src_len || !written)
        return WALLY_EINVAL;
    memcpy(bytes_out, src, src_len);
    *written = src_len;
    return WALLY_OK;
}

int wally_tx_input_get_txhash(const struct wally_tx_input *in,
                              unsigned char *bytes_out, size_t len)
{
    size_t written;
    if (len != WALLY_TXHASH_LEN)
        return WALLY_EINVAL;
    return tx_getb_impl(in, in->txhash,
                        WALLY_TXHASH_LEN, bytes_out, len, &written);
}

#define GET_TX_B(typ, name, siz) \
    int wally_ ## typ ## _get_ ## name(const struct wally_ ## typ *in, \
                                       unsigned char *bytes_out, size_t len, size_t * written) { \
        return tx_getb_impl(in, in->name, siz, bytes_out, len, written); \
    }

#define GET_TX_I(typ, name, outtyp) \
    int wally_ ## typ ## _get_ ## name(const struct wally_ ## typ *in, outtyp * written) { \
        if (written) *written = 0; \
        if (!in || !written) return WALLY_EINVAL; \
        *written = in->name; \
        return WALLY_OK; \
    }


GET_TX_B(tx_input, script, in->script_len)
static bool get_witness_preamble(const struct wally_tx_input *in,
                                 size_t index, size_t *written)
{
    if (written)
        *written = 0;
    if (!is_valid_tx_input(in) || !written ||
        !is_valid_witness_stack(in->witness) ||
        index >= in->witness->num_items)
        return false;
    return true;
}

int wally_tx_input_get_witness(const struct wally_tx_input *in, size_t index,
                               unsigned char *bytes_out, size_t len, size_t *written)
{
    if (!bytes_out || !get_witness_preamble(in, index, written) ||
        len < in->witness->items[index].witness_len)
        return WALLY_EINVAL;
    memcpy(bytes_out, in->witness->items[index].witness,
           in->witness->items[index].witness_len);
    *written = in->witness->items[index].witness_len;
    return WALLY_OK;
}

GET_TX_I(tx_input, index, size_t)
GET_TX_I(tx_input, sequence, size_t)
GET_TX_I(tx_input, script_len, size_t)

int wally_tx_input_get_witness_len(const struct wally_tx_input *in,
                                   size_t index, size_t *written)
{
    if (!get_witness_preamble(in, index, written))
        return WALLY_EINVAL;
    *written = in->witness->items[index].witness_len;
    return WALLY_OK;
}

GET_TX_B(tx_output, script, in->script_len)
GET_TX_I(tx_output, satoshi, uint64_t)
GET_TX_I(tx_output, script_len, size_t)

GET_TX_I(tx, version, size_t)
GET_TX_I(tx, locktime, size_t)
GET_TX_I(tx, num_inputs, size_t)
GET_TX_I(tx, num_outputs, size_t)

int wally_tx_output_set_script(struct wally_tx_output *out,
                               const unsigned char *script_in, size_t script_len_in)
{
    if (!is_valid_tx_output(out) || ((script_in != NULL) != (script_len_in != 0)))
        return WALLY_EINVAL;
    return replace_script(script_in, script_len_in, &out->script, &out->script_len);
}

int wally_tx_output_set_satoshi(struct wally_tx_output *out, uint64_t satoshi)
{
    if (!is_valid_tx_output(out))
        return WALLY_EINVAL;
    out->satoshi = satoshi;
    return WALLY_OK;
}

static struct wally_tx_input *tx_get_input(const struct wally_tx *tx, size_t index)
{
    return is_valid_tx(tx) && index < tx->num_inputs ? &tx->inputs[index] : NULL;
}

static struct wally_tx_output *tx_get_output(const struct wally_tx *tx, size_t index)
{
    return is_valid_tx(tx) && index < tx->num_outputs ? &tx->outputs[index] : NULL;
}

int wally_tx_get_input_script(const struct wally_tx *tx, size_t index,
                              unsigned char *bytes_out, size_t len, size_t *written)
{
    return wally_tx_input_get_script(tx_get_input(tx, index), bytes_out, len, written);
}

int wally_tx_get_input_script_len(const struct wally_tx *tx, size_t index, size_t *written)
{
    return wally_tx_input_get_script_len(tx_get_input(tx, index), written);
}

int wally_tx_get_input_witness(const struct wally_tx *tx, size_t index, size_t wit_index, unsigned char *bytes_out, size_t len, size_t *written)
{
    return wally_tx_input_get_witness(tx_get_input(tx, index), wit_index, bytes_out, len, written);
}

int wally_tx_get_input_witness_len(const struct wally_tx *tx, size_t index, size_t wit_index, size_t *written)
{
    return wally_tx_input_get_witness_len(tx_get_input(tx, index), wit_index, written);
}

int wally_tx_get_input_txhash(const struct wally_tx *tx, size_t index, unsigned char *bytes_out, size_t len)
{
    return wally_tx_input_get_txhash(tx_get_input(tx, index), bytes_out, len);
}

int wally_tx_get_input_index(const struct wally_tx *tx, size_t index, size_t *written)
{
    return wally_tx_input_get_index(tx_get_input(tx, index), written);
}

int wally_tx_get_input_sequence(const struct wally_tx *tx, size_t index, size_t *written)
{
    return wally_tx_input_get_sequence(tx_get_input(tx, index), written);
}

int wally_tx_get_output_script(const struct wally_tx *tx, size_t index,
                               unsigned char *bytes_out, size_t len, size_t *written)
{
    return wally_tx_output_get_script(tx_get_output(tx, index), bytes_out, len, written);
}

int wally_tx_get_output_script_len(const struct wally_tx *tx, size_t index, size_t *written)
{
    return wally_tx_output_get_script_len(tx_get_output(tx, index), written);
}

int wally_tx_get_output_satoshi(const struct wally_tx *tx, size_t index, uint64_t *value_out)
{
    return wally_tx_output_get_satoshi(tx_get_output(tx, index), value_out);
}

int wally_tx_set_input_script(const struct wally_tx *tx, size_t index,
                              const unsigned char *script_in, size_t script_len_in)
{
    struct wally_tx_input *in = tx_get_input(tx, index);

    if (!in || ((script_in != NULL) != (script_len_in != 0)))
        return WALLY_EINVAL;
    return replace_script(script_in, script_len_in, &in->script, &in->script_len);
}

int wally_tx_set_input_witness(const struct wally_tx *tx, size_t index,
                               const struct wally_tx_witness_stack *wit)
{
    struct wally_tx_input *in;
    struct wally_tx_witness_stack *new_witness = NULL;

    if (!(in = tx_get_input(tx, index)) || (wit && !is_valid_witness_stack(wit)))
        return WALLY_EINVAL;

    if (wit && (new_witness = clone_witness(wit)) == NULL)
        return WALLY_ENOMEM;

    tx_witness_stack_free(in->witness, true);
    in->witness = new_witness;
    return WALLY_OK;
}

int wally_tx_set_input_index(const struct wally_tx *tx, size_t index, uint32_t index_in)
{
    struct wally_tx_input *in = tx_get_input(tx, index);
    if (in)
        in->index = index_in;
    return in ? WALLY_OK : WALLY_EINVAL;
}

int wally_tx_set_input_sequence(const struct wally_tx *tx, size_t index, uint32_t sequence)
{
    struct wally_tx_input *in = tx_get_input(tx, index);
    if (in)
        in->sequence = sequence;
    return in ? WALLY_OK : WALLY_EINVAL;
}

int wally_tx_set_output_script(const struct wally_tx *tx, size_t index,
                               const unsigned char *script_in, size_t script_len_in)
{
    return wally_tx_output_set_script(tx_get_output(tx, index), script_in, script_len_in);
}

int wally_tx_set_output_satoshi(const struct wally_tx *tx, size_t index, uint64_t satoshi)
{
    return wally_tx_output_set_satoshi(tx_get_output(tx, index), satoshi);
}


#endif /* SWIG_JAVA_BUILD/SWIG_PYTHON_BUILD */
