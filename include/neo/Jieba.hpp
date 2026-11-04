#pragma once

#include "neo/Fwd.hpp" // IWYU pragma: export
#include "neo/algorithm/Output.hpp"
#include "neo/algorithm/PosTagging.hpp"
#include "neo/algorithm/Segmenter.hpp"
#include "neo/concept/Output.hpp"
#include "neo/concept/Text.hpp"    // IWYU pragma: export
#include "neo/options/CutMode.hpp" // IWYU pragma: export
#include "neo/options/Tagging.hpp" // IWYU pragma: export
#include "neo/options/UserWordWeightOption.hpp"
#include "neo/resource/DictSnapshot.hpp"
#include "neo/resource/DictTrie.hpp"
#include "neo/resource/HMModel.hpp"
#include "neo/util/FileIO.hpp"
#include "neo/util/Unicode.hpp"
#include "neo/value/DictSource.hpp"     // IWYU pragma: export
#include "neo/value/PosTag.hpp"         // IWYU pragma: export
#include "neo/value/Rune.hpp"           // IWYU pragma: export
#include "neo/value/SnapshotSource.hpp" // IWYU pragma: export
#include "neo/value/TaggedToken.hpp"    // IWYU pragma: export
#include "neo/value/TokenView.hpp"      // IWYU pragma: export
#include "neo/value/Tokens.hpp"         // IWYU pragma: export
#include "neo/workspace/Workspace.hpp"  // IWYU pragma: export

#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace neo_cppjieba {

/// Jieba is the top-level facade for Chinese word segmentation. It owns the dictionary and HMM model;
/// every segmentation entry point is const and keeps per-call state in a Workspace.
/// Runtime word edits are non-const and need exclusive access; the library takes no locks.
///
/// Entry points differ only in how results are delivered:
///   - cut / cut_with_workspace: positions plus a borrowed view of the input (Tokens).
///   - cut_owned: positions plus the moved-in input (OwnedTokens).
///   - cut_strings: independent strings in the input's encoding.
///   - cut_into / cut_each: caller-owned output vectors or a visitor, reusing a Workspace.
///   - cut_runes / cut_runes_into: predecoded runes in, rune ranges out.
/// The algorithm behind each CutMode is selected in algorithm/Segmenter.hpp.
class Jieba {
public:
    // An empty user dictionary path explicitly disables user dictionary loading.
    explicit Jieba(std::string_view dict_path, std::string_view model_path, std::string_view user_dict_path)
        : dict_(dict_path, user_dict_path, detail::UserWordWeightOption::WordWeightMedian), model_(model_path) {
    }

    // Load from files or memory; user dictionaries load in order and a repeated word takes the last entry.
    explicit Jieba(const DictSource &dict, const DictSource &model, std::span<const DictSource> user_dicts = {})
        : dict_(dict, user_dicts, detail::UserWordWeightOption::WordWeightMedian), model_(model) {
    }

    // Load the dictionary from a snapshot made by encode_dict_snapshot. It cuts exactly as the sources it was
    // encoded from would, with weights derived under this build's rules; errors use the checked exception.
    [[nodiscard]]
    static auto from_snapshot(const SnapshotSource &dict_snapshot, const DictSource &model) -> Jieba {
        return Jieba{detail::load_dict_snapshot(dict_snapshot), detail::HMModel{model}};
    }

    // Encode the dictionary the constructor taking these sources would build. Equal dictionaries give equal bytes,
    // and later builds keep reading the snapshots earlier builds wrote.
    [[nodiscard]]
    static auto encode_dict_snapshot(const DictSource &dict, std::span<const DictSource> user_dicts)
        -> std::vector<std::byte> {
        return detail::encode_dict_snapshot(
            detail::DictTrie::level_order(dict, user_dicts, detail::UserWordWeightOption::WordWeightMedian));
    }

    // Encode the snapshot and publish it at path atomically: readers see the previous file or the complete
    // snapshot, never a partial write, and a failure leaves the previous file in place.
    static auto write_dict_snapshot(const DictSource &dict, std::span<const DictSource> user_dicts,
                                    std::string_view path) -> void {
        detail::replace_file_atomically(path, encode_dict_snapshot(dict, user_dicts));
    }

    Jieba(Jieba &&) noexcept = default;
    auto operator=(Jieba &&) noexcept -> Jieba & = default;
    auto operator=(const Jieba &) -> Jieba & = delete;

    // An independent copy for copy-on-write updates: edit the clone, then publish it in place of the original.
    [[nodiscard]]
    auto clone() const -> Jieba {
        return Jieba{*this};
    }

    // Tagging::Pos makes an entry point attach each word's part-of-speech tag without changing segmentation:
    // the dictionary tag when the word has one, otherwise CppJieba's rule ("m", "eng" or "x").
    // Tags are looked up only after segmentation, so Tagging::None instantiations do no tagging work.

    // Borrowing rejects owning temporaries; callers keep the original text alive and stable.
    template <Tagging tagging = Tagging::None, StringViewSource Input>
    [[nodiscard]]
    auto cut(Input &&input, CutMode mode) const -> Tokens<output_char_type_t<Input>, tagging> {
        auto workspace = Workspace{};
        return cut_with_workspace<tagging>(std::forward<Input>(input), mode, workspace);
    }

    // Caller-owned workspace may be reused; each result owns its positions and borrows only the input.
    // Convert the input once so decoding and the returned view refer to the same source.
    template <Tagging tagging = Tagging::None, StringViewSource Input>
    [[nodiscard]]
    auto cut_with_workspace(Input &&input, CutMode mode, Workspace &workspace) const
        -> Tokens<output_char_type_t<Input>, tagging> {
        const auto source = detail::as_view(std::forward<Input>(input));
        auto positions = std::vector<detail::token_record_t<tagging>>{};
        cut_into(source, mode, positions, workspace);
        return Tokens<output_char_type_t<Input>, tagging>{source, std::move(positions)};
    }

    // Replaces output while retaining its capacity. Output records contain no decoding-buffer pointers.
    // Caller-owned decoding buffers can be reused across inputs; results never borrow this storage.
    // Input must not alias the output array or either decoding buffer.
    // The workspace also retains route, HMM, separator, and rune-result storage.
    // TaggedPosition and TaggedWord outputs select tagging; other outputs are untagged.
    template <StringLike Input, typename Output>
        requires detail::OutputValue<Output, output_char_type_t<Input>>
                 || detail::TaggedOutputValue<Output, output_char_type_t<Input>>
    auto cut_into(const Input &input, CutMode mode, std::vector<Output> &out, Workspace &workspace) const -> void {
        out.clear();
        const auto source = detail::as_view(input);
        const auto &state = segment_source(source, mode, workspace);
        if constexpr (detail::TaggedOutputValue<Output, output_char_type_t<Input>>) {
            detail::fill_tagged_output(dict_, source, state.decoded.runes, state.ranges, state.decoded.offsets, out);
        } else {
            detail::fill_output(source, state.ranges, state.decoded.offsets, out);
        }
    }

    // No public token array is built; segmenters still compute rune ranges first.
    // Visitors may throw, but must not reuse this decoding buffer or invalidate the input text.
    template <Tagging tagging = Tagging::None, StringLike Input, typename Emit>
        requires detail::ViewVisitor<Emit, detail::token_view_t<tagging, output_char_type_t<Input>>>
    auto cut_each(const Input &input, CutMode mode, Emit &&emit, Workspace &workspace) const -> void {
        const auto source = detail::as_view(input);
        const auto &state = segment_source(source, mode, workspace);
        if constexpr (tagging == Tagging::Pos) {
            detail::emit_tagged_tokens(dict_, source, state.decoded.runes, state.ranges, state.decoded.offsets, emit);
        } else {
            detail::emit_tokens(source, state.ranges, state.decoded.offsets, emit);
        }
    }

    // Move the source into its final owner before building positions, including for small strings.
    template <Tagging tagging = Tagging::None, CharType CharT>
    [[nodiscard]]
    auto cut_owned(std::basic_string<CharT> source, CutMode mode) const -> OwnedTokens<CharT, tagging> {
        auto result = OwnedTokens<CharT, tagging>{std::move(source), {}};
        auto workspace = Workspace{};
        cut_into(result.source_, mode, result.positions_, workspace);
        return result;
    }

    // UTF-8 byte buffers produce std::string words; character inputs retain their native string type.
    // Tagging::Pos returns word/tag pairs, like CppJieba's Tag and Python jieba's posseg.lcut.
    template <Tagging tagging = Tagging::None, StringLike Input>
    [[nodiscard]]
    auto cut_strings(const Input &input, CutMode mode) const
        -> std::vector<detail::word_t<tagging, output_char_type_t<Input>>> {
        auto workspace = Workspace{};
        auto result = std::vector<detail::word_t<tagging, output_char_type_t<Input>>>{};
        cut_into(input, mode, result, workspace);
        return result;
    }

    // Predecoded input retains rune coordinates and never constructs source text or an offset table.
    [[nodiscard]]
    auto cut_runes(std::span<const Rune> runes, CutMode mode) const -> std::vector<WordRange> {
        auto result = std::vector<WordRange>{};
        auto workspace = Workspace{};
        cut_runes_into(runes, mode, result, workspace);
        return result;
    }

    // Fill the caller's rune output directly; decoding storage is unused for predecoded input.
    // A failure propagates and may leave partial output.
    auto cut_runes_into(std::span<const Rune> runes, CutMode mode, std::vector<WordRange> &out,
                        Workspace &workspace) const -> void {
        out.clear();
        detail::check_cut_mode(mode);
        detail::assert_rune_count(runes.size());
        detail::segment_into(dict_, model_, runes, mode, out, detail::segment_state(workspace).scratch);
    }

    // Tag one complete word without segmenting it, like CppJieba's LookupTag.
    [[nodiscard]]
    auto lookup_tag(std::span<const Rune> word) const noexcept -> PosTag {
        return detail::word_tag(dict_, word);
    }

    template <StringLike Input>
    [[nodiscard]]
    auto lookup_tag(const Input &word) const -> PosTag {
        const auto runes = detail::decode(word);
        return lookup_tag(std::span<const Rune>{runes});
    }

    // Runtime words behave like user dictionary entries and keep the load-time frequency total.
    // Callers serialize edits against all other calls, e.g. with a lock, copy-on-write, or a single thread.
    template <StringLike Input>
    auto add_word(const Input &word, PosTag tag) -> void {
        const auto runes = detail::decode(word);
        dict_.add_word(runes, tag);
    }

    template <StringLike Input>
    auto add_word_with_freq(const Input &word, int freq, PosTag tag) -> void {
        const auto runes = detail::decode(word);
        dict_.add_word_with_freq(runes, freq, tag);
    }

    // Removal drops main and user dictionary words alike; returns false when the word is absent.
    template <StringLike Input>
    auto remove_word(const Input &word) -> bool {
        const auto runes = detail::decode(word);
        return dict_.remove_word(runes);
    }

    // Reclaim storage left by runtime edits; segmentation results are unchanged.
    auto compact() -> void {
        dict_.compact();
    }

private:
    // Decode and segment into the workspace; the returned state holds the runes, offsets and word ranges.
    template <CharType CharT>
    auto segment_source(std::basic_string_view<CharT> source, CutMode mode, Workspace &workspace) const
        -> const detail::SegmentState & {
        detail::check_cut_mode(mode);
        auto &state = detail::segment_state(workspace);
        detail::decode_with_offset_into(source, state.decoded);
        // Decoding bounds source offsets; the rune count cannot exceed the source code-unit count.
        // Rune and source capacities may be configured independently.
        detail::assert_rune_count(state.decoded.runes.size());
        detail::segment_into(dict_, model_, state.decoded.runes, mode, state.ranges, state.scratch);
        return state;
    }

    // Snapshot loading builds the dictionary directly; the model still loads from its source.
    Jieba(detail::DictTrie dict, detail::HMModel model) noexcept : dict_(std::move(dict)), model_(std::move(model)) {
    }

    // Copying is reserved for clone() so whole-dictionary copies stay explicit at call sites.
    Jieba(const Jieba &other) : dict_(other.dict_.clone()), model_(other.model_.clone()) {
    }

    detail::DictTrie dict_;
    detail::HMModel model_;
};

} // namespace neo_cppjieba
