#include <cstddef>
#include <cstdint>

#include <linkerbot/modbus/rtu.hpp>

// The symbol name and signature are the libFuzzer entry point.
// NOLINTNEXTLINE(readability-identifier-naming)
extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size == 0 || size > 256) {
        return 0;
    }
    const uint8_t functions[]{3, 4, 6, 16};
    linkerbot::modbus::Request request;
    request.unit_id = static_cast<uint8_t>(1U + data[0] % 247U);
    request.function = functions[data[0] % 4U];
    if (request.function == 6 || request.function == 16) {
        request.values = {size > 1 ? uint16_t{data[1]} : uint16_t{0}};
    }
    (void)linkerbot::modbus::encode_request(request);
    try {
        (void)linkerbot::modbus::decode_response(request, {data, data + size});
    } catch (const linkerbot::modbus::ProtocolError &) {
        return 0;
    }
    return 0;
}
