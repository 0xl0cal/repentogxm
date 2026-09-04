/* Portable game-owned utility boundaries.
 *
 * The original split helpers construct an MSVC basic_stringstream, repeatedly
 * call the game's std::getline wrapper, and immediately destroy the stream.
 * No stream object escapes either function.  Reproducing the resulting byte
 * split here keeps the guest-visible std::string/vector ABI while avoiding
 * native MSVCP vtable and locale pointers that cannot enter guest dispatch. */
#include "manual_portable.h"
#include "guest_funcs.h"
#include "guest_coverage_generated.h"

#define GUEST_STRING_BYTES 24u
#define GUEST_STRING_LENGTH 0x10u
#define GUEST_STRING_CAPACITY 0x14u
#define GUEST_STRING_SSO_CAPACITY 15u

#define SPLIT_APPEND_RVA 0x00259650u
#define SPLIT_CONSTRUCT_RVA 0x002597b0u
#define PATH_FROM_BASE_RVA 0x002599c0u
#define PERFORMANCE_COUNTERS_RVA 0x002c8700u
#define PATH_COPY_BYTES 0x104u
#define PATH_INPUT_LIMIT 0x100000u

static int split_fault(CPU *__restrict c, uint32_t rva, const char *message)
{
    guest_fault(c, rva, message);
    return 0;
}

static int exact_return(CPU *__restrict c, uint32_t saved, uint32_t rva,
                        const char *operation)
{
    if (c->fault)
        return 0;
    if (c->esp != saved)
        return split_fault(c, rva, operation);
    return 1;
}

static int string_copy_construct(CPU *__restrict c, uint32_t dst,
                                 uint32_t src, uint32_t rva)
{
    uint32_t saved = c->esp;
    gpush(c, src);
    gpush(c, 0xabc079f0u);
    c->ecx = dst;
    sub_000079f0(c);
    return exact_return(c, saved, rva,
                        "portable split: string copy ABI drift");
}

static int string_assign_bytes(CPU *__restrict c, uint32_t dst,
                               uint32_t bytes, uint32_t length, uint32_t rva)
{
    uint32_t saved = c->esp;
    gpush(c, length);
    gpush(c, bytes);
    gpush(c, 0xabc07c80u);
    c->ecx = dst;
    sub_00007c80(c);
    return exact_return(c, saved, rva,
                        "portable split: string assign ABI drift");
}

static int string_destroy(CPU *__restrict c, uint32_t string, uint32_t rva)
{
    uint32_t saved = c->esp;
    gpush(c, 0xabc07be0u);
    c->ecx = string;
    sub_00007be0(c);
    return exact_return(c, saved, rva,
                        "portable split: string destructor ABI drift");
}

static int validate_vector(CPU *__restrict c, uint32_t vector, uint32_t rva)
{
    uint32_t begin = ld32(vector);
    uint32_t end = ld32(vector + 4u);
    uint32_t capacity = ld32(vector + 8u);

    if ((begin == 0u && (end != 0u || capacity != 0u)) ||
        begin > end || end > capacity ||
        (end - begin) % GUEST_STRING_BYTES != 0u ||
        (capacity - begin) % GUEST_STRING_BYTES != 0u)
        return split_fault(c, rva,
                           "portable split: malformed vector<string>");
    return 1;
}

static int vector_append(CPU *__restrict c, uint32_t vector,
                         uint32_t string, uint32_t rva)
{
    uint32_t end, capacity, saved;
    if (!validate_vector(c, vector, rva))
        return 0;
    end = ld32(vector + 4u);
    capacity = ld32(vector + 8u);
    saved = c->esp;

    if (end != capacity) {
        if (capacity - end < GUEST_STRING_BYTES)
            return split_fault(c, rva,
                               "portable split: partial vector capacity");
        gpush(c, string);
        gpush(c, 0xabc079f0u);
        c->ecx = end;
        sub_000079f0(c);
        if (!exact_return(c, saved, rva,
                          "portable split: vector copy ABI drift"))
            return 0;
        st32(vector + 4u, end + GUEST_STRING_BYTES);
        return 1;
    }

    /* vector::_Emplace_reallocate(this, old_end, source), exact x86 ABI. */
    gpush(c, string);
    gpush(c, end);
    gpush(c, 0xabc25cebu);
    c->ecx = vector;
    sub_0025ceb0(c);
    return exact_return(c, saved, rva,
                        "portable split: vector grow ABI drift");
}

static int validate_source(CPU *__restrict c, uint32_t source, uint32_t rva)
{
    uint32_t length, capacity, bytes;
    if (!source)
        return split_fault(c, rva, "portable split: null source string");
    length = ld32(source + GUEST_STRING_LENGTH);
    capacity = ld32(source + GUEST_STRING_CAPACITY);
    if (length > 0x7fffffffu)
        return split_fault(c, rva,
                           "portable split: source exceeds MSVC string max");
    if (capacity < length)
        return split_fault(c, rva,
                           "portable split: malformed source string");
    bytes = capacity < 16u ? source : ld32(source);
    if (length && !bytes)
        return split_fault(c, rva,
                           "portable split: null source storage");
    return 1;
}

static int split_bytes(CPU *__restrict c, uint32_t source, uint32_t vector,
                       uint8_t delimiter, int construct_vector, uint32_t rva)
{
    uint32_t entry_sp = c->esp;
    uint32_t saved_ebp = c->ebp, saved_ebx = c->ebx;
    uint32_t saved_esi = c->esi, saved_edi = c->edi;
    uint32_t snapshot, token, bytes, length, position;

    if (!vector)
        return split_fault(c, rva, "portable split: null output vector");
    if (entry_sp < 2u * GUEST_STRING_BYTES)
        return split_fault(c, rva, "portable split: guest stack underflow");

    /* SplitConstruct is a vector constructor, not assignment: the original
     * zeroes its three pointers before it even constructs the stringstream. */
    if (construct_vector) {
        st32(vector, 0u);
        st32(vector + 4u, 0u);
        st32(vector + 8u, 0u);
    }
    if (!validate_source(c, source, rva) ||
        (!construct_vector && !validate_vector(c, vector, rva)))
        return 0;

    /* The original stringstream snapshots the complete source before the
     * first vector append.  This matters when source aliases an element of a
     * full output vector and the append reallocates that vector. */
    snapshot = entry_sp - 2u * GUEST_STRING_BYTES;
    token = snapshot + GUEST_STRING_BYTES;
    if (!guest_stack_set(c, snapshot, rva))
        return 0;
    if (!string_copy_construct(c, snapshot, source, rva))
        return 0;

    st32(token, 0u);
    st32(token + GUEST_STRING_LENGTH, 0u);
    st32(token + GUEST_STRING_CAPACITY, GUEST_STRING_SSO_CAPACITY);
    st8(token, 0u);

    length = ld32(snapshot + GUEST_STRING_LENGTH);
    bytes = ld32(snapshot + GUEST_STRING_CAPACITY) < 16u
        ? snapshot : ld32(snapshot);
    position = 0u;
    while (position < length) {
        uint32_t start = position;
        while (position < length && ld8(bytes + position) != delimiter)
            ++position;
        if (!string_assign_bytes(c, token, bytes + start,
                                 position - start, rva) ||
            !vector_append(c, vector, token, rva))
            return 0;
        if (position == length)
            break;
        ++position;                 /* getline consumes the delimiter */
        if (position == length)
            break;                  /* no synthetic field after a trailing one */
    }

    if (!string_destroy(c, token, rva) ||
        !string_destroy(c, snapshot, rva))
        return 0;

    c->ebp = saved_ebp;
    c->ebx = saved_ebx;
    c->esi = saved_esi;
    c->edi = saved_edi;
    if (!guest_stack_set(c, entry_sp, rva))
        return 0;
    c->eax = vector;
    (void)gpop(c);                  /* both wrappers use a plain RET */
    return 1;
}

void sub_00259650(CPU *__restrict c)
{
    uint32_t source;
    uint32_t vector;
    uint8_t delimiter;
    guest_coverage_function(
        GUEST_COVERAGE_MANUAL_PORTABLE_SPLIT_APPEND_ID);
    source = c->ecx;
    vector = ld32(guest_stack_address(
        c, c->esp + 4u, 4U, SPLIT_APPEND_RVA));
    delimiter = (uint8_t)c->edx;
    (void)split_bytes(c, source, vector, delimiter, 0, SPLIT_APPEND_RVA);
}

void sub_002597b0(CPU *__restrict c)
{
    uint32_t vector;
    uint32_t source;
    uint8_t delimiter;
    guest_coverage_function(
        GUEST_COVERAGE_MANUAL_PORTABLE_SPLIT_CONSTRUCT_ID);
    vector = c->ecx;
    source = c->edx;
    delimiter = ld8(guest_stack_address(
        c, c->esp + 4u, 1U, SPLIT_CONSTRUCT_RVA));
    (void)split_bytes(c, source, vector, delimiter, 1, SPLIT_CONSTRUCT_RVA);
}

typedef struct PathPiece {
    uint32_t source;
    uint32_t length;
} PathPiece;

typedef struct PathIterator {
    uint32_t source;
    uint32_t length;
    uint32_t position;
} PathIterator;

/* Only the prefix which can affect strncpy's 260 output bytes is retained.
 * Later components are represented by a depth: a following ".." consumes
 * that hidden tail before it can affect the retained prefix.  Since every
 * retained component contributes at least its slash, the array cannot fill
 * before the rendered prefix reaches PATH_COPY_BYTES. */
typedef struct PathStack {
    PathPiece pieces[PATH_COPY_BYTES];
    uint16_t emitted_before[PATH_COPY_BYTES];
    uint32_t count;
    uint32_t hidden_count;
    uint32_t emitted;
} PathStack;

static int path_fault(CPU *__restrict c, const char *message)
{
    guest_fault(c, PATH_FROM_BASE_RVA, message);
    return 0;
}

static int path_measure(CPU *__restrict c, uint32_t source,
                        uint32_t *length_out, const char *null_message,
                        const char *limit_message)
{
    uint32_t length = 0u;

    if (!source)
        return path_fault(c, null_message);
    for (;;) {
        uint8_t value;
        if (source > 0xffffffffu - length)
            return path_fault(c, limit_message);
        value = ld8(source + length);
        if (!value)
            break;
        if (length == PATH_INPUT_LIMIT)
            return path_fault(c, limit_message);
        ++length;
    }
    *length_out = length;
    return 1;
}

static int path_is_delimiter(uint8_t value)
{
    return value == (uint8_t)'/' || value == (uint8_t)'\\';
}

static int path_next(PathIterator *iterator, PathPiece *piece)
{
    uint32_t start;

    if (iterator->position >= iterator->length)
        return 0;
    start = iterator->position;
    while (iterator->position < iterator->length &&
           !path_is_delimiter(
               ld8(iterator->source + iterator->position)))
        ++iterator->position;
    piece->source = iterator->source + start;
    piece->length = iterator->position - start;
    if (iterator->position < iterator->length)
        ++iterator->position;          /* split() suppresses a trailing field */
    return 1;
}

static void path_stack_push(PathStack *stack, PathPiece piece)
{
    uint32_t remaining;

    if (stack->hidden_count) {
        ++stack->hidden_count;
        return;
    }
    if (stack->emitted >= PATH_COPY_BYTES) {
        stack->hidden_count = 1u;
        return;
    }

    stack->emitted_before[stack->count] = (uint16_t)stack->emitted;
    stack->pieces[stack->count++] = piece;
    remaining = PATH_COPY_BYTES - stack->emitted;
    if (piece.length >= remaining)
        stack->emitted = PATH_COPY_BYTES;
    else
        stack->emitted += piece.length + 1u; /* component plus slash */
}

static int path_stack_pop(PathStack *stack)
{
    if (stack->hidden_count) {
        --stack->hidden_count;
        return 1;
    }
    if (!stack->count)
        return 0;
    --stack->count;
    stack->emitted = stack->emitted_before[stack->count];
    return 1;
}

static int path_piece_is_parent(const PathPiece *piece)
{
    return piece->length == 2u &&
           ld8(piece->source) == (uint8_t)'.' &&
           ld8(piece->source + 1u) == (uint8_t)'.';
}

static void path_render_piece(uint8_t rendered[PATH_COPY_BYTES],
                              uint32_t *logical, const PathPiece *piece)
{
    uint32_t i;
    for (i = 0u; i < piece->length && *logical < PATH_COPY_BYTES; ++i) {
        uint8_t value = ld8(piece->source + i);
        rendered[(*logical)++] =
            value == (uint8_t)'\\' ? (uint8_t)'/' : value;
    }
}

static int path_from_base(CPU *__restrict c, uint32_t base_source,
                          uint32_t relative_source, uint32_t output)
{
    uint32_t entry_sp = c->esp;
    uint32_t saved_ebp = c->ebp, saved_ebx = c->ebx;
    uint32_t saved_esi = c->esi, saved_edi = c->edi;
    uint32_t base_length = 0u, relative_length = 0u;
    uint32_t i, logical = 0u;
    PathIterator base_iterator, relative_iterator;
    PathPiece current, following, leaf;
    PathStack stack = {0};
    uint8_t rendered[PATH_COPY_BYTES];

    if (!output || output > 0xffffffffu - (PATH_COPY_BYTES - 1u))
        return path_fault(c, "portable path: invalid output range");
    /* The translated CPU/dispatcher is single-thread-owned and native
     * audio/input workers do not re-enter guest code, so guest path bytes
     * cannot be mutated while this handler runs.  The measure/stream passes
     * are deliberately not a transactional view of unsupported foreign
     * concurrent writes. */
    if (!path_measure(c, base_source, &base_length,
                      "portable path: null base path",
                      "portable path: base path exceeds snapshot limit") ||
        !path_measure(c, relative_source, &relative_length,
                      "portable path: null relative path",
                      "portable path: relative path exceeds snapshot limit"))
        return 0;

    base_iterator.source = base_source;
    base_iterator.length = base_length;
    base_iterator.position = 0u;
    if (!path_next(&base_iterator, &current))
        return path_fault(c, "portable path: empty path has no leaf component");
    while (path_next(&base_iterator, &following)) {
        path_stack_push(&stack, current);
        current = following;
    }                                   /* current is the excluded base leaf */

    relative_iterator.source = relative_source;
    relative_iterator.length = relative_length;
    relative_iterator.position = 0u;
    if (!path_next(&relative_iterator, &current))
        return path_fault(c, "portable path: empty path has no leaf component");
    while (path_next(&relative_iterator, &following)) {
        if (path_piece_is_parent(&current)) {
            if (!path_stack_pop(&stack))
                path_stack_push(&stack, current);
        } else {
            path_stack_push(&stack, current);
        }
        current = following;
    }
    leaf = current;

    /* Rendering finishes before the first guest store, so output may alias
     * either input without changing a later component or leaf read. */
    for (i = 0u; i < stack.count && logical < PATH_COPY_BYTES; ++i) {
        path_render_piece(rendered, &logical, &stack.pieces[i]);
        if (logical < PATH_COPY_BYTES)
            rendered[logical++] = (uint8_t)'/';
    }
    path_render_piece(rendered, &logical, &leaf);
    while (logical < PATH_COPY_BYTES)
        rendered[logical++] = 0u;       /* exact strncpy padding */
    for (i = 0u; i < PATH_COPY_BYTES; ++i)
        st8(output + i, rendered[i]);

    c->ebp = saved_ebp;
    c->ebx = saved_ebx;
    c->esi = saved_esi;
    c->edi = saved_edi;
    if (!guest_stack_set(c, entry_sp, PATH_FROM_BASE_RVA))
        return 0;
    (void)gpop(c);                      /* plain RET; caller removes 8 bytes */
    return 1;
}

void sub_002599c0(CPU *__restrict c)
{
    guest_coverage_function(
        GUEST_COVERAGE_MANUAL_PORTABLE_PATH_FROM_BASE_ID);
    (void)path_from_base(
        c, c->ecx, c->edx,
        ld32(guest_stack_address(
            c, c->esp + 4u, 4U, PATH_FROM_BASE_RVA)));
}

void sub_002c8700(CPU *__restrict c)
{
    guest_coverage_function(
        GUEST_COVERAGE_MANUAL_PORTABLE_PERFORMANCE_COUNTERS_ID);
    /* The original body only formats two performance counters and sends the
     * resulting informational strings to the game logger.  No stream or
     * formatted buffer escapes, and no game state is changed. */
    (void)PERFORMANCE_COUNTERS_RVA;
    (void)gpop(c);
}
