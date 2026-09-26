#pragma once
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <unordered_set>

namespace eidolon {
// Controller-owned correlation state. Callers must serialize access; media
// callbacks carry Fence() back to the actor rather than mutate actor state.
// Authorization, session binding and output policy are checked before Arm.
class AudioPresentationState {
public:
    enum class Phase { Idle, Armed, Streaming, Sealed, Draining, Completed, Failed };
    static constexpr size_t kMaxBytes = 16000 * 2 * 120;

    bool Arm(const std::string& turn, const std::string& stream, uint64_t epoch) {
        if ((phase_ != Phase::Idle && phase_ != Phase::Completed) ||
            turn.empty() || stream.empty() || turn.size() > 128 || stream.size() > 128 ||
            !epoch || epoch < epoch_ || fence_ == std::numeric_limits<uint32_t>::max()) return false;
        if (epoch > epoch_) seen_.clear();
        if (seen_.size() >= 32 || seen_.count(turn)) return false;
        seen_.insert(turn);
        epoch_ = epoch;
        ++fence_;
        turn_ = turn;
        stream_ = stream;
        next_chunk_ = bytes_ = 0;
        phase_ = Phase::Armed;
        return true;
    }
    bool Open(const std::string& stream, bool trusted_sender, bool is_text) {
        if (stream != stream_) return false;
        if (phase_ != Phase::Armed || !trusted_sender || is_text) return Fail();
        phase_ = Phase::Streaming;
        return true;
    }
    bool Chunk(const std::string& stream, uint64_t index, size_t size) {
        if (stream != stream_) return false;
        if (phase_ != Phase::Streaming || index != next_chunk_ || size > 15000 ||
            size % 2 || size > kMaxBytes - bytes_) return Fail();
        ++next_chunk_;
        bytes_ += size;
        return true;
    }
    bool Seal(const std::string& stream, bool normal_trailer) {
        if (stream != stream_) return false;
        if (phase_ != Phase::Streaming || !normal_trailer || !bytes_) return Fail();
        phase_ = Phase::Sealed;
        return true;
    }
    bool BeginDrain(uint32_t renderer_eos_fence) {
        if (renderer_eos_fence != fence_ || phase_ != Phase::Sealed) return false;
        phase_ = Phase::Draining;
        return true;
    }
    bool Complete(uint32_t drain_fence, bool hardware_drained) {
        if (drain_fence != fence_ || phase_ != Phase::Draining) return false;
        if (!hardware_drained) return Fail();
        phase_ = Phase::Completed;
        return true;
    }
    void Cancel() {
        // Revoke first, then caller flushes renderer and queued stream chunks.
        if (fence_ != std::numeric_limits<uint32_t>::max()) ++fence_;
        stream_.clear();
        turn_.clear();
        bytes_ = next_chunk_ = 0;
        phase_ = Phase::Idle;
    }
    void ResetScene() {
        Cancel();
        epoch_ = 0;
        seen_.clear();
    }
    uint32_t Fence() const { return fence_; }
    size_t Bytes() const { return bytes_; }
    Phase State() const { return phase_; }
private:
    bool Fail() { phase_ = Phase::Failed; return false; }
    Phase phase_ = Phase::Idle;
    uint32_t fence_ = 0;
    uint64_t epoch_ = 0, next_chunk_ = 0;
    size_t bytes_ = 0;
    std::string turn_, stream_;
    std::unordered_set<std::string> seen_;
};
} // namespace eidolon
