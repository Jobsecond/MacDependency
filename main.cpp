#include <fstream>
#include <iostream>
#include <string>
#include <vector>
#include <type_traits>

#include <mach-o/loader.h>
#include <mach-o/fat.h>
#include <mach-o/arch.h>


// ANSI escape codes for text formatting
#define ANSI_COLOR_GREEN "\x1b[32m"
#define ANSI_COLOR_BLUE "\x1b[34m"
#define ANSI_COLOR_BOLD "\x1b[1m"
#define ANSI_COLOR_RESET "\x1b[0m"


struct MachOInfo {
    std::string arch;
    std::string dylib_id;
    std::vector<std::string> deps;
    std::vector<std::string> rpaths;
};

void printInformation(const std::string &name);

template <bool is64BitMachHeader>
bool parseMachHeaderAndUpdateResult(std::ifstream &file,
                                    std::ifstream::pos_type pos,
                                    std::vector<MachOInfo> &result);

template <bool is64BitFatArch>
void parseFatHeaderAndUpdateResult(std::ifstream &file,
                                   std::vector<MachOInfo> &result);

std::vector<MachOInfo> parseMachO(const std::string &filename);


// IMPLEMENTATION BELOW

int main(int argc, char **argv) {
    if (argc < 2) {
        std::cout << "Usage: " << argv[0] << " <mach-o> [<mach-o> ...]\n";
        return 1;
    }
    for (int i = 1; i < argc; i++) {
        printInformation(argv[i]);
        std::cout << '\n';
    }

    return 0;
}

void printInformation(const std::string &name) {
    auto result = parseMachO(name);
    std::cout << ANSI_COLOR_BOLD << ANSI_COLOR_BLUE << "- filename: " << ANSI_COLOR_RESET << name << '\n';
    std::cout << ANSI_COLOR_BOLD << ANSI_COLOR_BLUE << "  info: " << ANSI_COLOR_RESET << '\n';
    for (const auto &item : result) {
        std::cout << ANSI_COLOR_BOLD << ANSI_COLOR_GREEN << "  - arch: " << ANSI_COLOR_RESET << item.arch << '\n';
        if (!item.dylib_id.empty()) {
            std::cout << ANSI_COLOR_BOLD << ANSI_COLOR_GREEN << "    dylib_id: " << ANSI_COLOR_RESET
                      << item.dylib_id << '\n';
        }
        std::cout << ANSI_COLOR_BOLD << ANSI_COLOR_GREEN << "    deps: " << ANSI_COLOR_RESET << '\n';
        for (const auto &dep : item.deps) {
            std::cout << "    - " << dep << '\n';
        }
        std::cout << ANSI_COLOR_BOLD << ANSI_COLOR_GREEN << "    rpaths: " << ANSI_COLOR_RESET << '\n';
        for (const auto &rpath : item.rpaths) {
            std::cout << "    - " << rpath << '\n';
        }
    }
}

template <bool is64BitMachHeader>
bool parseMachHeaderAndUpdateResult(std::ifstream &file,
                                    std::ifstream::pos_type pos,
                                    std::vector<MachOInfo> &result) {
    using MachHeaderType = typename std::conditional<is64BitMachHeader, struct mach_header_64, struct mach_header>::type;
    MachHeaderType mh {};
    file.seekg(pos);
    file.read(reinterpret_cast<char*>(&mh), sizeof(MachHeaderType));

    // Get architecture name
    const auto arch = NXGetArchInfoFromCpuType(mh.cputype, mh.cpusubtype);
    if (!arch) {
        std::cout << "Unable to get architecture name\n";
        return false;  // break the switch statement
    }

    MachOInfo machOInfo;
    machOInfo.arch = arch->name;

    uint32_t ncmds = mh.ncmds;
    uint32_t sizeofcmds = mh.sizeofcmds;

    std::vector<char> cmds(sizeofcmds);
    file.read(cmds.data(), sizeofcmds);

    size_t arrIndex = 0;
    for (uint32_t i = 0; i < ncmds; i++) {
        if (arrIndex > cmds.size() || arrIndex + sizeof(struct load_command) > cmds.size()) {
            // Array boundary check
            break;
        }
        auto ptr = &cmds[arrIndex];
        auto lc = reinterpret_cast<struct load_command *>(ptr);
        uint32_t cmd = lc->cmd;
        uint32_t cmdsize = lc->cmdsize;

        if (cmd == LC_LOAD_DYLIB || cmd == LC_LOAD_WEAK_DYLIB) {
            auto cmd_struct = reinterpret_cast<struct dylib_command *>(ptr);
            auto name = (char *)cmd_struct + cmd_struct->dylib.name.offset;
            machOInfo.deps.emplace_back(name);
        } else if (cmd == LC_RPATH) {
            auto cmd_struct = reinterpret_cast<struct rpath_command *>(ptr);
            auto rpath = (char *)cmd_struct + cmd_struct->path.offset;
            machOInfo.rpaths.emplace_back(rpath);
        } else if (cmd == LC_ID_DYLIB) {
            auto cmd_struct = reinterpret_cast<struct dylib_command *>(ptr);
            auto name = (char *)cmd_struct + cmd_struct->dylib.name.offset;
            machOInfo.dylib_id = name;
        }

        arrIndex += cmdsize;
    }

    result.emplace_back(std::move(machOInfo));
    return true;
}

template <bool is64BitFatArch>
void parseFatHeaderAndUpdateResult(std::ifstream &file,
                                   std::vector<MachOInfo> &result) {
    using FatArchType = typename std::conditional<is64BitFatArch, struct fat_arch_64, struct fat_arch>::type;

    // Fat binary (universal binary), 32-bit header
    struct fat_header fh {};
    file.read(reinterpret_cast<char*>(&fh), sizeof(struct fat_header));
    // Swap byte order, since all fields in the universal header are big-endian.
    fh.nfat_arch = OSSwapInt32(fh.nfat_arch);

    for (uint32_t i = 0; i < fh.nfat_arch; i++) {
        // Read architecture info
        FatArchType fa {};
        file.seekg(sizeof(struct fat_header) + i * sizeof(FatArchType));
        file.read(reinterpret_cast<char*>(&fa), sizeof(FatArchType));
        fa.cputype = OSSwapInt32(fa.cputype);
        fa.cpusubtype = OSSwapInt32(fa.cpusubtype);
        if constexpr(is64BitFatArch) {
            fa.offset = OSSwapInt64(fa.offset);
        } else {
            fa.offset = OSSwapInt32(fa.offset);
        }
        // Get architecture name
        const NXArchInfo *arch = NXGetArchInfoFromCpuType(fa.cputype, fa.cpusubtype);
        if (!arch) {
            std::cout << "Unable to get architecture name\n";
            continue;  // continue for loop
        }

        // Navigate to the beginning of architecture
        file.seekg(fa.offset);
        // Read the magic number of architecture
        uint32_t magic;
        file.read(reinterpret_cast<char*>(&magic), sizeof(uint32_t));

        file.seekg(fa.offset);

        if (magic == MH_MAGIC_64) {
            constexpr bool is64BitMachHeader = true;
            parseMachHeaderAndUpdateResult<is64BitMachHeader>(file, fa.offset, result);
        } else {
            constexpr bool is64BitMachHeader = false;
            parseMachHeaderAndUpdateResult<is64BitMachHeader>(file, fa.offset, result);
        }
    }
}

std::vector<MachOInfo> parseMachO(const std::string &filename) {
    // Open Mach-O File
    std::ifstream file(filename, std::ios::binary | std::ios::in);
    if (!file.is_open()) {
        std::cout << "Could not open file: " << filename << '\n';
        return {};
    }

    std::vector<MachOInfo> result;

    // Read file header to determine if it's a Mach-O file
    uint32_t magic;
    file.read(reinterpret_cast<char*>(&magic), sizeof(uint32_t));
    file.seekg(0, std::ios::beg);

    // Check the magic number
    switch (magic) {
        // Check if it's a fat binary (universal binary)
        case FAT_MAGIC:
        case FAT_CIGAM:
        {
            // Fat binary (universal binary), 32-bit header
            constexpr bool is64BitFatArch = false;
            parseFatHeaderAndUpdateResult<is64BitFatArch>(file, result);
        } // cases for fat binaries
            break;
        case FAT_MAGIC_64:
        case FAT_CIGAM_64:
        {
            // Fat binary (universal binary), 64-bit header
            constexpr bool is64BitFatArch = true;
            parseFatHeaderAndUpdateResult<is64BitFatArch>(file, result);
        } // cases for fat binaries
            break;
        case MH_MAGIC:
        case MH_CIGAM:
        {
            // Not a fat binary, only one architecture
            // 32-bit
            constexpr bool is64BitMachHeader = false;
            parseMachHeaderAndUpdateResult<is64BitMachHeader>(file, 0, result);
        } // cases for thin binaries
            break;
        case MH_MAGIC_64:
        case MH_CIGAM_64:
        {
            // Not a fat binary, only one architecture
            // 64-bit
            constexpr bool is64BitMachHeader = true;
            parseMachHeaderAndUpdateResult<is64BitMachHeader>(file, 0, result);
        } // cases for thin binaries
            break;
        default:
            std::cout << "File " << filename << " is not a Mach-O file\n";
            return {};
    }
    return result;
}
