#ifndef EIDOLON_PENDING_ROOM_JOIN_H_
#define EIDOLON_PENDING_ROOM_JOIN_H_

#include <optional>
#include <utility>
#include "control_protocol.h"

namespace eidolon {

// One outstanding room.join on the controller's actor task. Keeps its receipt
// owner until completion; another command cannot overwrite it during connect.
class PendingRoomJoin {
public:
    enum class Admission { Started, Retry, Busy, Conflict };

    Admission Begin(const ControlCommand& command, const std::string& intent)
    {
        if (command_) {
            if (command.id.empty() || command.id != command_->id) return Admission::Busy;
            return intent == intent_ ? Admission::Retry : Admission::Conflict;
        }
        command_ = command;
        intent_ = intent;
        generation_ = 0;
        return Admission::Started;
    }

    void BindGeneration(uint32_t generation)
    {
        if (command_) generation_ = generation;
    }

    bool active() const { return command_.has_value(); }

    bool Matches(uint32_t generation) const
    {
        return command_ && (generation_ == 0 || generation_ == generation);
    }

    std::optional<ControlCommand> Complete()
    {
        auto command = std::move(command_);
        command_.reset();
        intent_.clear();
        generation_ = 0;
        return command;
    }

private:
    std::optional<ControlCommand> command_;
    std::string intent_;
    uint32_t generation_ = 0;
};

} // namespace eidolon
#endif
