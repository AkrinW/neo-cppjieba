# Fail when a per-rune hot helper has an out-of-line copy in the inline probe, which GCC built with no inline budget.
# Each entry is kept inline by NEO_FORCE_INLINE or by a direct lambda call that the early inliner removes.
set(hot_helpers
    "decode_utf8_fast<"
    "HMModel::get_emit_probs\\("
    "is_default_separator_unlikely\\("
)

execute_process(COMMAND "${NM}" -C "${BINARY}" OUTPUT_VARIABLE symbols RESULT_VARIABLE status)
if(NOT status EQUAL 0)
    message(FATAL_ERROR "${NM} failed on ${BINARY}")
endif()

set(out_of_line)
foreach(pattern IN LISTS hot_helpers)
    string(REGEX MATCH "[^\n]*${pattern}[^\n]*" symbol "${symbols}")
    if(symbol)
        list(APPEND out_of_line "${symbol}")
    endif()
endforeach()
if(out_of_line)
    list(JOIN out_of_line "\n  " lines)
    message(FATAL_ERROR "Per-rune helpers left out of line with no inline budget:\n  ${lines}")
endif()
