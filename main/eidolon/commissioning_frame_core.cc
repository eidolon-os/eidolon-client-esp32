#include "eidolon/commissioning_frame_core.h"

#include <algorithm>

#include "eidolon/device_foundation_v1_generated.h"

namespace eidolon {
namespace {

namespace df = device_foundation::v1;
using Frame = df::EndpointFrame;
using Op = df::EndpointFrameOp;
using Status = df::EndpointFrameStatus;

std::uint16_t U16(const std::uint8_t* p)
{
    return static_cast<std::uint16_t>((p[0] << 8) | p[1]);
}

std::uint32_t U32(const std::uint8_t* p)
{
    return (static_cast<std::uint32_t>(p[0]) << 24) | (static_cast<std::uint32_t>(p[1]) << 16) |
           (static_cast<std::uint32_t>(p[2]) << 8) | static_cast<std::uint32_t>(p[3]);
}

void PutU16(std::string& out, std::uint16_t value)
{
    out.push_back(static_cast<char>(value >> 8));
    out.push_back(static_cast<char>(value & 0xff));
}

void PutU32(std::string& out, std::uint32_t value)
{
    for (int shift = 24; shift >= 0; shift -= 8) out.push_back(static_cast<char>((value >> shift) & 0xff));
}

EndpointFrameReply Reply(Status status, std::uint16_t transfer_id, std::uint16_t length = 0,
                         std::uint32_t total = 0, std::uint32_t offset = 0,
                         const char* chunk = nullptr, std::size_t chunk_length = 0)
{
    EndpointFrameReply reply;
    reply.frame.reserve(Frame::kHeaderBytes + chunk_length);
    reply.frame.push_back(static_cast<char>(Frame::kMagic0));
    reply.frame.push_back(static_cast<char>(Frame::kMagic1));
    reply.frame.push_back(static_cast<char>(Frame::kVersion));
    reply.frame.push_back(static_cast<char>(status));
    PutU16(reply.frame, transfer_id);
    PutU16(reply.frame, length);
    PutU32(reply.frame, total);
    PutU32(reply.frame, offset);
    if (chunk_length > 0) reply.frame.append(chunk, chunk_length);
    return reply;
}

}  // namespace

bool IsEndpointFrame(const std::uint8_t* data, std::size_t length)
{
    return length >= 2 && data[0] == Frame::kMagic0 && data[1] == Frame::kMagic1;
}

std::size_t EndpointFrameRequestLimit(std::string_view endpoint)
{
    if (endpoint == "eidolon-descriptor") return Frame::kDescriptorRequestLimit;
    if (endpoint == "eidolon-status") return Frame::kStatusRequestLimit;
    if (endpoint == "eidolon-terminal-ack") return Frame::kTerminalAckRequestLimit;
    if (endpoint == "eidolon-trust") return Frame::kTrustRequestLimit;
    return 0;
}

EndpointFrameServer::EndpointFrameServer(std::size_t request_limit) : limit_(request_limit) {}

void EndpointFrameServer::Reset()
{
    transfer_id_ = 0;
    total_ = 0;
    request_.clear();
    request_.shrink_to_fit();
    response_.clear();
    response_.shrink_to_fit();
    have_response_ = false;
    pending_ = false;
    delivered_ = 0;
}

EndpointFrameReply EndpointFrameServer::Handle(const std::uint8_t* data, std::size_t length,
                                               const EndpointRequestHandler& handler,
                                               std::uint16_t retry_after_ms)
{
    if (length < Frame::kHeaderBytes || length > Frame::kMaxFrameBytes ||
        !IsEndpointFrame(data, length)) {
        return Reply(Status::Malformed, 0);
    }
    if (data[2] != Frame::kVersion) return Reply(Status::UnsupportedVersion, 0);
    const auto op = data[3];
    const std::uint16_t transfer_id = U16(data + 4);
    const std::uint16_t field_length = U16(data + 6);
    const std::uint32_t total = U32(data + 8);
    const std::uint32_t offset = U32(data + 12);
    const std::uint8_t* chunk = data + Frame::kHeaderBytes;
    const std::size_t chunk_length = length - Frame::kHeaderBytes;
    if (transfer_id == 0) return Reply(Status::Malformed, 0);

    if (op == static_cast<std::uint8_t>(Op::Abort)) {
        if (transfer_id != transfer_id_) return Reply(Status::UnknownTransfer, transfer_id);
        Reset();
        return Reply(Status::Continue, transfer_id);
    }
    if (op == static_cast<std::uint8_t>(Op::Put)) {
        return Put(transfer_id, field_length, total, offset, chunk, chunk_length, handler,
                   retry_after_ms);
    }
    if (op == static_cast<std::uint8_t>(Op::Get)) {
        return Get(transfer_id, field_length, total, offset, chunk_length, handler,
                   retry_after_ms);
    }
    return Reply(Status::Malformed, transfer_id);
}

EndpointFrameReply EndpointFrameServer::Put(std::uint16_t transfer_id, std::uint16_t length,
                                            std::uint32_t total, std::uint32_t offset,
                                            const std::uint8_t* chunk, std::size_t chunk_length,
                                            const EndpointRequestHandler& handler,
                                            std::uint16_t retry_after_ms)
{
    if (length != chunk_length || length == 0) return Reply(Status::Malformed, transfer_id);
    if (offset == 0) {
        if (transfer_id_ != 0) return Reply(Status::Busy, transfer_id);
        if (total == 0 || total > limit_) return Reply(Status::TooLarge, transfer_id);
        transfer_id_ = transfer_id;
        total_ = total;
        request_.reserve(total);
    } else if (transfer_id != transfer_id_) {
        return Reply(Status::UnknownTransfer, transfer_id);
    }
    if (total != total_ || offset != request_.size() || have_response_ || pending_ ||
        static_cast<std::uint64_t>(offset) + length > total_) {
        Reset();
        return Reply(Status::Sequence, transfer_id);
    }
    request_.append(reinterpret_cast<const char*>(chunk), chunk_length);
    if (request_.size() < total_) {
        return Reply(Status::Continue, transfer_id, 0, 0, static_cast<std::uint32_t>(request_.size()));
    }
    const EndpointRequestResult result = handler(&request_);
    return Respond(result, transfer_id, retry_after_ms);
}

EndpointFrameReply EndpointFrameServer::Respond(const EndpointRequestResult& result,
                                                std::uint16_t transfer_id,
                                                std::uint16_t retry_after_ms)
{
    switch (result.outcome) {
    case EndpointRequestOutcome::Failed: {
        Reset();
        EndpointFrameReply reply;
        reply.handler_failed = true;
        return reply;
    }
    case EndpointRequestOutcome::Pending:
        pending_ = true;
        return Reply(Status::Pending, transfer_id, retry_after_ms);
    case EndpointRequestOutcome::Ready:
        break;
    }
    pending_ = false;
    have_response_ = true;
    response_ = result.response;
    request_.clear();
    request_.shrink_to_fit();
    return Reply(Status::Ready, transfer_id, 0, static_cast<std::uint32_t>(response_.size()));
}

EndpointFrameReply EndpointFrameServer::Get(std::uint16_t transfer_id, std::uint16_t length,
                                            std::uint32_t total, std::uint32_t offset,
                                            std::size_t chunk_length,
                                            const EndpointRequestHandler& handler,
                                            std::uint16_t retry_after_ms)
{
    if (transfer_id != transfer_id_) return Reply(Status::UnknownTransfer, transfer_id);
    if (chunk_length != 0 || total != 0 || length == 0) return Reply(Status::Malformed, transfer_id);
    if (pending_) {
        const EndpointRequestResult result = handler(nullptr);
        if (result.outcome != EndpointRequestOutcome::Ready) {
            return Respond(result, transfer_id, retry_after_ms);
        }
        pending_ = false;
        have_response_ = true;
        response_ = result.response;
        request_.clear();
        request_.shrink_to_fit();
    }
    if (!have_response_ || offset != delivered_) {
        Reset();
        return Reply(Status::Sequence, transfer_id);
    }
    const std::size_t remaining = response_.size() - delivered_;
    const std::size_t size =
        std::min<std::size_t>({static_cast<std::size_t>(length), Frame::kMaxChunkBytes, remaining});
    EndpointFrameReply reply =
        Reply(Status::Data, transfer_id, static_cast<std::uint16_t>(size),
              static_cast<std::uint32_t>(response_.size()), delivered_,
              response_.data() + delivered_, size);
    delivered_ += static_cast<std::uint32_t>(size);
    if (delivered_ == response_.size()) Reset();
    return reply;
}

}  // namespace eidolon
