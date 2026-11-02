/* Freeze the C ABI v1: a change to any listed type, layout, value or function type fails to compile.
 * Releases only add lines here; editing an existing line means the ABI changed and needs a new major version.
 * Layouts are checked for 64-bit targets, the platform ABIs the C API ships for. */

#include "neo_cppjieba.h"

#include <stddef.h>
#include <stdint.h>

/* ---- Scalar types and numeric values ---------------------------------------------------------------------------- */

_Static_assert(_Generic((neo_jieba_status_v1)0, int32_t: 1, default: 0), "neo_jieba_status_v1 is int32_t");
_Static_assert(_Generic((neo_jieba_mode_v1)0, uint32_t: 1, default: 0), "neo_jieba_mode_v1 is uint32_t");
_Static_assert(_Generic((neo_jieba_source_kind_v1)0, uint32_t: 1, default: 0), "neo_jieba_source_kind_v1 is uint32_t");

_Static_assert(NEO_JIEBA_V1_OK == 0, "status OK");
_Static_assert(NEO_JIEBA_V1_INVALID_ARGUMENT == 1, "status INVALID_ARGUMENT");
_Static_assert(NEO_JIEBA_V1_OPERATION_FAILED == 2, "status OPERATION_FAILED");
_Static_assert(NEO_JIEBA_V1_OUT_OF_MEMORY == 3, "status OUT_OF_MEMORY");
_Static_assert(NEO_JIEBA_V1_LIMIT_EXCEEDED == 4, "status LIMIT_EXCEEDED");

_Static_assert(NEO_JIEBA_V1_MIX == 1, "mode MIX");
_Static_assert(NEO_JIEBA_V1_MIX_NO_HMM == 2, "mode MIX_NO_HMM");
_Static_assert(NEO_JIEBA_V1_FULL == 3, "mode FULL");
_Static_assert(NEO_JIEBA_V1_SEARCH == 4, "mode SEARCH");
_Static_assert(NEO_JIEBA_V1_SEARCH_NO_HMM == 5, "mode SEARCH_NO_HMM");
_Static_assert(NEO_JIEBA_V1_HMM == 6, "mode HMM");
_Static_assert(NEO_JIEBA_V1_MP == 7, "mode MP");

_Static_assert(NEO_JIEBA_V1_SOURCE_FILE == 1, "source kind FILE");
_Static_assert(NEO_JIEBA_V1_SOURCE_MEMORY == 2, "source kind MEMORY");

/* ---- Field types ------------------------------------------------------------------------------------------------ */

_Static_assert(_Generic(((neo_jieba_paths_v1 *)0)->dict_path, const char *: 1, default: 0), "paths.dict_path");
_Static_assert(_Generic(((neo_jieba_paths_v1 *)0)->model_path, const char *: 1, default: 0), "paths.model_path");
_Static_assert(_Generic(((neo_jieba_paths_v1 *)0)->user_dict_path, const char *: 1, default: 0),
               "paths.user_dict_path");

_Static_assert(_Generic(((neo_jieba_source_v1 *)0)->kind, neo_jieba_source_kind_v1: 1, default: 0), "source.kind");
_Static_assert(_Generic(((neo_jieba_source_v1 *)0)->reserved, uint32_t: 1, default: 0), "source.reserved");
_Static_assert(_Generic(((neo_jieba_source_v1 *)0)->data, const char *: 1, default: 0), "source.data");
_Static_assert(_Generic(((neo_jieba_source_v1 *)0)->size, uint64_t: 1, default: 0), "source.size");

_Static_assert(_Generic(((neo_jieba_error_buffer_v1 *)0)->data, char *: 1, default: 0), "error_buffer.data");
_Static_assert(_Generic(((neo_jieba_error_buffer_v1 *)0)->capacity, uint64_t: 1, default: 0), "error_buffer.capacity");
_Static_assert(_Generic(((neo_jieba_error_buffer_v1 *)0)->message_size, uint64_t: 1, default: 0),
               "error_buffer.message_size");

_Static_assert(_Generic(((neo_jieba_token_range_v1 *)0)->byte_offset, uint64_t: 1, default: 0),
               "token_range.byte_offset");
_Static_assert(_Generic(((neo_jieba_token_range_v1 *)0)->byte_length, uint64_t: 1, default: 0),
               "token_range.byte_length");

_Static_assert(sizeof(((neo_jieba_pos_tag_v1 *)0)->data) == 4, "pos_tag.data is char[4]");
_Static_assert(_Generic(((neo_jieba_pos_tag_v1 *)0)->data[0], char: 1, default: 0), "pos_tag.data elements");

_Static_assert(_Generic(((neo_jieba_borrowed_tokens_v1 *)0)->utf8_data, const char *: 1, default: 0),
               "borrowed_tokens.utf8_data");
_Static_assert(_Generic(((neo_jieba_borrowed_tokens_v1 *)0)->utf8_size, uint64_t: 1, default: 0),
               "borrowed_tokens.utf8_size");
_Static_assert(_Generic(((neo_jieba_borrowed_tokens_v1 *)0)->ranges, const neo_jieba_token_range_v1 *: 1, default: 0),
               "borrowed_tokens.ranges");
_Static_assert(_Generic(((neo_jieba_borrowed_tokens_v1 *)0)->count, uint64_t: 1, default: 0), "borrowed_tokens.count");

_Static_assert(_Generic(((neo_jieba_borrowed_ranges_v1 *)0)->ranges, const neo_jieba_token_range_v1 *: 1, default: 0),
               "borrowed_ranges.ranges");
_Static_assert(_Generic(((neo_jieba_borrowed_ranges_v1 *)0)->count, uint64_t: 1, default: 0), "borrowed_ranges.count");
_Static_assert(_Generic(((neo_jieba_borrowed_ranges_v1 *)0)->tags, const neo_jieba_pos_tag_v1 *: 1, default: 0),
               "borrowed_ranges.tags");

_Static_assert(_Generic(((neo_jieba_tfidf_options_v1 *)0)->allowed_tags, const neo_jieba_pos_tag_v1 *: 1, default: 0),
               "tfidf_options.allowed_tags");
_Static_assert(_Generic(((neo_jieba_tfidf_options_v1 *)0)->allowed_tag_count, uint64_t: 1, default: 0),
               "tfidf_options.allowed_tag_count");

_Static_assert(_Generic(((neo_jieba_textrank_options_v1 *)0)->window, uint64_t: 1, default: 0),
               "textrank_options.window");
_Static_assert(_Generic(((neo_jieba_textrank_options_v1 *)0)->iterations, uint64_t: 1, default: 0),
               "textrank_options.iterations");
_Static_assert(_Generic(((neo_jieba_textrank_options_v1 *)0)->damping, double: 1, default: 0),
               "textrank_options.damping");
_Static_assert(_Generic(((neo_jieba_textrank_options_v1 *)0)->allowed_tags,
                   const neo_jieba_pos_tag_v1 *: 1,
                   default: 0),
               "textrank_options.allowed_tags");
_Static_assert(_Generic(((neo_jieba_textrank_options_v1 *)0)->allowed_tag_count, uint64_t: 1, default: 0),
               "textrank_options.allowed_tag_count");

_Static_assert(_Generic(((neo_jieba_keyword_v1 *)0)->weight, double: 1, default: 0), "keyword.weight");
_Static_assert(_Generic(((neo_jieba_keyword_v1 *)0)->first_occurrence, uint64_t: 1, default: 0),
               "keyword.first_occurrence");
_Static_assert(_Generic(((neo_jieba_keyword_v1 *)0)->occurrence_count, uint64_t: 1, default: 0),
               "keyword.occurrence_count");
_Static_assert(_Generic(((neo_jieba_keyword_v1 *)0)->tag, neo_jieba_pos_tag_v1: 1, default: 0), "keyword.tag");
_Static_assert(_Generic(((neo_jieba_keyword_v1 *)0)->reserved, uint32_t: 1, default: 0), "keyword.reserved");

_Static_assert(_Generic(((neo_jieba_borrowed_keywords_v1 *)0)->keywords, const neo_jieba_keyword_v1 *: 1, default: 0),
               "borrowed_keywords.keywords");
_Static_assert(_Generic(((neo_jieba_borrowed_keywords_v1 *)0)->count, uint64_t: 1, default: 0),
               "borrowed_keywords.count");
_Static_assert(_Generic(((neo_jieba_borrowed_keywords_v1 *)0)->occurrences,
                   const neo_jieba_token_range_v1 *: 1,
                   default: 0),
               "borrowed_keywords.occurrences");
_Static_assert(_Generic(((neo_jieba_borrowed_keywords_v1 *)0)->occurrence_count, uint64_t: 1, default: 0),
               "borrowed_keywords.occurrence_count");

/* ---- Layouts of the 64-bit platform ABIs ------------------------------------------------------------------------ */

#if UINTPTR_MAX == UINT64_MAX
_Static_assert(sizeof(neo_jieba_paths_v1) == 24 && _Alignof(neo_jieba_paths_v1) == 8, "paths layout");
_Static_assert(offsetof(neo_jieba_paths_v1, dict_path) == 0 && offsetof(neo_jieba_paths_v1, model_path) == 8 &&
                   offsetof(neo_jieba_paths_v1, user_dict_path) == 16,
               "paths offsets");

_Static_assert(sizeof(neo_jieba_source_v1) == 24 && _Alignof(neo_jieba_source_v1) == 8, "source layout");
_Static_assert(offsetof(neo_jieba_source_v1, kind) == 0 && offsetof(neo_jieba_source_v1, reserved) == 4 &&
                   offsetof(neo_jieba_source_v1, data) == 8 && offsetof(neo_jieba_source_v1, size) == 16,
               "source offsets");

_Static_assert(sizeof(neo_jieba_error_buffer_v1) == 24 && _Alignof(neo_jieba_error_buffer_v1) == 8,
               "error_buffer layout");
_Static_assert(offsetof(neo_jieba_error_buffer_v1, data) == 0 && offsetof(neo_jieba_error_buffer_v1, capacity) == 8 &&
                   offsetof(neo_jieba_error_buffer_v1, message_size) == 16,
               "error_buffer offsets");

_Static_assert(sizeof(neo_jieba_token_range_v1) == 16 && _Alignof(neo_jieba_token_range_v1) == 8, "token_range layout");
_Static_assert(offsetof(neo_jieba_token_range_v1, byte_offset) == 0 &&
                   offsetof(neo_jieba_token_range_v1, byte_length) == 8,
               "token_range offsets");

_Static_assert(sizeof(neo_jieba_pos_tag_v1) == 4 && _Alignof(neo_jieba_pos_tag_v1) == 1, "pos_tag layout");

_Static_assert(sizeof(neo_jieba_borrowed_tokens_v1) == 32 && _Alignof(neo_jieba_borrowed_tokens_v1) == 8,
               "borrowed_tokens layout");
_Static_assert(offsetof(neo_jieba_borrowed_tokens_v1, utf8_data) == 0 &&
                   offsetof(neo_jieba_borrowed_tokens_v1, utf8_size) == 8 &&
                   offsetof(neo_jieba_borrowed_tokens_v1, ranges) == 16 &&
                   offsetof(neo_jieba_borrowed_tokens_v1, count) == 24,
               "borrowed_tokens offsets");

_Static_assert(sizeof(neo_jieba_borrowed_ranges_v1) == 24 && _Alignof(neo_jieba_borrowed_ranges_v1) == 8,
               "borrowed_ranges layout");
_Static_assert(offsetof(neo_jieba_borrowed_ranges_v1, ranges) == 0 &&
                   offsetof(neo_jieba_borrowed_ranges_v1, count) == 8 &&
                   offsetof(neo_jieba_borrowed_ranges_v1, tags) == 16,
               "borrowed_ranges offsets");

_Static_assert(sizeof(neo_jieba_tfidf_options_v1) == 16 && _Alignof(neo_jieba_tfidf_options_v1) == 8,
               "tfidf_options layout");
_Static_assert(offsetof(neo_jieba_tfidf_options_v1, allowed_tags) == 0 &&
                   offsetof(neo_jieba_tfidf_options_v1, allowed_tag_count) == 8,
               "tfidf_options offsets");

_Static_assert(sizeof(neo_jieba_textrank_options_v1) == 40 && _Alignof(neo_jieba_textrank_options_v1) == 8,
               "textrank_options layout");
_Static_assert(offsetof(neo_jieba_textrank_options_v1, window) == 0 &&
                   offsetof(neo_jieba_textrank_options_v1, iterations) == 8 &&
                   offsetof(neo_jieba_textrank_options_v1, damping) == 16 &&
                   offsetof(neo_jieba_textrank_options_v1, allowed_tags) == 24 &&
                   offsetof(neo_jieba_textrank_options_v1, allowed_tag_count) == 32,
               "textrank_options offsets");

_Static_assert(sizeof(neo_jieba_keyword_v1) == 32 && _Alignof(neo_jieba_keyword_v1) == 8, "keyword layout");
_Static_assert(offsetof(neo_jieba_keyword_v1, weight) == 0 && offsetof(neo_jieba_keyword_v1, first_occurrence) == 8 &&
                   offsetof(neo_jieba_keyword_v1, occurrence_count) == 16 &&
                   offsetof(neo_jieba_keyword_v1, tag) == 24 && offsetof(neo_jieba_keyword_v1, reserved) == 28,
               "keyword offsets");

_Static_assert(sizeof(neo_jieba_borrowed_keywords_v1) == 32 && _Alignof(neo_jieba_borrowed_keywords_v1) == 8,
               "borrowed_keywords layout");
_Static_assert(offsetof(neo_jieba_borrowed_keywords_v1, keywords) == 0 &&
                   offsetof(neo_jieba_borrowed_keywords_v1, count) == 8 &&
                   offsetof(neo_jieba_borrowed_keywords_v1, occurrences) == 16 &&
                   offsetof(neo_jieba_borrowed_keywords_v1, occurrence_count) == 24,
               "borrowed_keywords offsets");
#endif

/* ---- Function types: version 1.0 -------------------------------------------------------------------------------- */

_Static_assert(_Generic(&neo_jieba_v1_max_input_bytes, uint64_t (*)(void): 1, default: 0), "max_input_bytes");
_Static_assert(_Generic(&neo_jieba_v1_engine_create,
                   neo_jieba_status_v1 (*)(const neo_jieba_paths_v1 *, neo_jieba_engine_v1 **,
                                           neo_jieba_error_buffer_v1 *): 1,
                   default: 0),
               "engine_create");
_Static_assert(_Generic(&neo_jieba_v1_engine_destroy, void (*)(neo_jieba_engine_v1 *): 1, default: 0),
               "engine_destroy");
_Static_assert(_Generic(&neo_jieba_v1_cut_utf8_owned,
                   neo_jieba_status_v1 (*)(const neo_jieba_engine_v1 *, const char *, uint64_t, neo_jieba_mode_v1,
                                           neo_jieba_tokens_v1 **, neo_jieba_error_buffer_v1 *): 1,
                   default: 0),
               "cut_utf8_owned");
_Static_assert(_Generic(&neo_jieba_v1_tokens_borrow_data,
                   neo_jieba_status_v1 (*)(const neo_jieba_tokens_v1 *, neo_jieba_borrowed_tokens_v1 *,
                                           neo_jieba_error_buffer_v1 *): 1,
                   default: 0),
               "tokens_borrow_data");
_Static_assert(_Generic(&neo_jieba_v1_tokens_destroy, void (*)(neo_jieba_tokens_v1 *): 1, default: 0),
               "tokens_destroy");

/* ---- Function types: version 1.1 -------------------------------------------------------------------------------- */

_Static_assert(_Generic(&neo_jieba_v1_engine_create_from_sources,
                   neo_jieba_status_v1 (*)(const neo_jieba_source_v1 *, const neo_jieba_source_v1 *,
                                           const neo_jieba_source_v1 *, uint64_t, neo_jieba_engine_v1 **,
                                           neo_jieba_error_buffer_v1 *): 1,
                   default: 0),
               "engine_create_from_sources");
_Static_assert(_Generic(&neo_jieba_v1_engine_create_from_snapshot,
                   neo_jieba_status_v1 (*)(const neo_jieba_source_v1 *, const neo_jieba_source_v1 *,
                                           neo_jieba_engine_v1 **, neo_jieba_error_buffer_v1 *): 1,
                   default: 0),
               "engine_create_from_snapshot");
_Static_assert(_Generic(&neo_jieba_v1_write_dict_snapshot,
                   neo_jieba_status_v1 (*)(const neo_jieba_source_v1 *, const neo_jieba_source_v1 *, uint64_t,
                                           const char *, uint64_t, neo_jieba_error_buffer_v1 *): 1,
                   default: 0),
               "write_dict_snapshot");
_Static_assert(_Generic(&neo_jieba_v1_engine_clone,
                   neo_jieba_status_v1 (*)(const neo_jieba_engine_v1 *, neo_jieba_engine_v1 **,
                                           neo_jieba_error_buffer_v1 *): 1,
                   default: 0),
               "engine_clone");
_Static_assert(_Generic(&neo_jieba_v1_engine_add_word,
                   neo_jieba_status_v1 (*)(neo_jieba_engine_v1 *, const char *, uint64_t, const neo_jieba_pos_tag_v1 *,
                                           neo_jieba_error_buffer_v1 *): 1,
                   default: 0),
               "engine_add_word");
_Static_assert(_Generic(&neo_jieba_v1_engine_add_word_with_freq,
                   neo_jieba_status_v1 (*)(neo_jieba_engine_v1 *, const char *, uint64_t, int32_t,
                                           const neo_jieba_pos_tag_v1 *, neo_jieba_error_buffer_v1 *): 1,
                   default: 0),
               "engine_add_word_with_freq");
_Static_assert(_Generic(&neo_jieba_v1_engine_remove_word,
                   neo_jieba_status_v1 (*)(neo_jieba_engine_v1 *, const char *, uint64_t, uint32_t *,
                                           neo_jieba_error_buffer_v1 *): 1,
                   default: 0),
               "engine_remove_word");
_Static_assert(_Generic(&neo_jieba_v1_engine_compact,
                   neo_jieba_status_v1 (*)(neo_jieba_engine_v1 *, neo_jieba_error_buffer_v1 *): 1,
                   default: 0),
               "engine_compact");
_Static_assert(_Generic(&neo_jieba_v1_workspace_create,
                   neo_jieba_status_v1 (*)(neo_jieba_workspace_v1 **, neo_jieba_error_buffer_v1 *): 1,
                   default: 0),
               "workspace_create");
_Static_assert(_Generic(&neo_jieba_v1_workspace_destroy, void (*)(neo_jieba_workspace_v1 *): 1, default: 0),
               "workspace_destroy");
_Static_assert(_Generic(&neo_jieba_v1_ranges_create,
                   neo_jieba_status_v1 (*)(neo_jieba_ranges_v1 **, neo_jieba_error_buffer_v1 *): 1,
                   default: 0),
               "ranges_create");
_Static_assert(_Generic(&neo_jieba_v1_ranges_destroy, void (*)(neo_jieba_ranges_v1 *): 1, default: 0),
               "ranges_destroy");
_Static_assert(_Generic(&neo_jieba_v1_cut_utf8_into,
                   neo_jieba_status_v1 (*)(const neo_jieba_engine_v1 *, const char *, uint64_t, neo_jieba_mode_v1,
                                           neo_jieba_workspace_v1 *, neo_jieba_ranges_v1 *,
                                           neo_jieba_error_buffer_v1 *): 1,
                   default: 0),
               "cut_utf8_into");
_Static_assert(_Generic(&neo_jieba_v1_cut_utf8_tagged_into,
                   neo_jieba_status_v1 (*)(const neo_jieba_engine_v1 *, const char *, uint64_t, neo_jieba_mode_v1,
                                           neo_jieba_workspace_v1 *, neo_jieba_ranges_v1 *,
                                           neo_jieba_error_buffer_v1 *): 1,
                   default: 0),
               "cut_utf8_tagged_into");
_Static_assert(_Generic(&neo_jieba_v1_ranges_borrow_data,
                   neo_jieba_status_v1 (*)(const neo_jieba_ranges_v1 *, neo_jieba_borrowed_ranges_v1 *,
                                           neo_jieba_error_buffer_v1 *): 1,
                   default: 0),
               "ranges_borrow_data");
_Static_assert(_Generic(&neo_jieba_v1_lookup_tag,
                   neo_jieba_status_v1 (*)(const neo_jieba_engine_v1 *, const char *, uint64_t, neo_jieba_pos_tag_v1 *,
                                           neo_jieba_error_buffer_v1 *): 1,
                   default: 0),
               "lookup_tag");
_Static_assert(_Generic(&neo_jieba_v1_tfidf_create,
                   neo_jieba_status_v1 (*)(const neo_jieba_source_v1 *, const neo_jieba_source_v1 *,
                                           neo_jieba_tfidf_v1 **, neo_jieba_error_buffer_v1 *): 1,
                   default: 0),
               "tfidf_create");
_Static_assert(_Generic(&neo_jieba_v1_tfidf_destroy, void (*)(neo_jieba_tfidf_v1 *): 1, default: 0), "tfidf_destroy");
_Static_assert(_Generic(&neo_jieba_v1_textrank_create,
                   neo_jieba_status_v1 (*)(const neo_jieba_source_v1 *, neo_jieba_textrank_v1 **,
                                           neo_jieba_error_buffer_v1 *): 1,
                   default: 0),
               "textrank_create");
_Static_assert(_Generic(&neo_jieba_v1_textrank_destroy, void (*)(neo_jieba_textrank_v1 *): 1, default: 0),
               "textrank_destroy");
_Static_assert(_Generic(&neo_jieba_v1_keywords_create,
                   neo_jieba_status_v1 (*)(neo_jieba_keywords_v1 **, neo_jieba_error_buffer_v1 *): 1,
                   default: 0),
               "keywords_create");
_Static_assert(_Generic(&neo_jieba_v1_keywords_destroy, void (*)(neo_jieba_keywords_v1 *): 1, default: 0),
               "keywords_destroy");
_Static_assert(_Generic(&neo_jieba_v1_tfidf_extract_into,
                   neo_jieba_status_v1 (*)(const neo_jieba_tfidf_v1 *, const neo_jieba_engine_v1 *, const char *,
                                           uint64_t, uint64_t, const neo_jieba_tfidf_options_v1 *,
                                           neo_jieba_workspace_v1 *, neo_jieba_keywords_v1 *,
                                           neo_jieba_error_buffer_v1 *): 1,
                   default: 0),
               "tfidf_extract_into");
_Static_assert(_Generic(&neo_jieba_v1_textrank_extract_into,
                   neo_jieba_status_v1 (*)(const neo_jieba_textrank_v1 *, const neo_jieba_engine_v1 *, const char *,
                                           uint64_t, uint64_t, const neo_jieba_textrank_options_v1 *,
                                           neo_jieba_workspace_v1 *, neo_jieba_keywords_v1 *,
                                           neo_jieba_error_buffer_v1 *): 1,
                   default: 0),
               "textrank_extract_into");
_Static_assert(_Generic(&neo_jieba_v1_keywords_borrow_data,
                   neo_jieba_status_v1 (*)(const neo_jieba_keywords_v1 *, neo_jieba_borrowed_keywords_v1 *,
                                           neo_jieba_error_buffer_v1 *): 1,
                   default: 0),
               "keywords_borrow_data");
