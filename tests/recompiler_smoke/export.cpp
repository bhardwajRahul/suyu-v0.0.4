#include "core/recompiler/arm64_to_c.h"
#include "code.h"

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    const auto stats = suyu::recomp::EmitProject(
        "smoke", reinterpret_cast<const uint8_t*>(smoke_code), sizeof(smoke_code),
        0x1000, argv[1], true);
    const auto second = suyu::recomp::EmitProject(
        "second", reinterpret_cast<const uint8_t*>(smoke_code), sizeof(smoke_code),
        0x1000, std::string(argv[1]) + "/second", true);
    return stats.unhandled == 0 && stats.emitted == sizeof(smoke_code) / 4 &&
                   second.unhandled == 0 && second.emitted == sizeof(smoke_code) / 4
               ? 0
               : 1;
}
