#include <fstream>
#include <iostream>
#include <string>
#include <vector>
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
    std::vector<std::string> deps;
    std::vector<std::string> rpaths;
};

template <typename MachHeaderType>
void readAndPrintDependencies(std::ifstream &file, const MachHeaderType &mh, struct MachOInfo &machOInfo) {
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
            auto dc = reinterpret_cast<struct dylib_command *>(ptr);
            auto name = (char *)dc + dc->dylib.name.offset;
            machOInfo.deps.emplace_back(name);
        } else if (cmd == LC_RPATH) {
            auto rc = reinterpret_cast<struct rpath_command *>(ptr);
            auto rpath = (char *)rc + rc->path.offset;
            machOInfo.rpaths.emplace_back(rpath);
        }

        arrIndex += cmdsize;
    }
}

std::vector<struct MachOInfo> parseMachO(const std::string &filename) {
    // Open Mach-O File
    std::ifstream file(filename, std::ios::binary | std::ios::in);
    if (!file.is_open()) {
        std::cout << "Could not open file: " << filename << '\n';
        return {};
    }

    std::vector<struct MachOInfo> result;

    // Read file header to determine if it's a Mach-O file
    uint32_t magic;
    file.read(reinterpret_cast<char*>(&magic), sizeof(uint32_t));
    file.seekg(0, std::ios::beg);

    // Check if it's a fat binary (universal binary)
    switch (magic) {
        case FAT_MAGIC:
        case FAT_MAGIC_64:
        case FAT_CIGAM:
        case FAT_CIGAM_64:
        {
            // Fat binary (universal binary)
            struct fat_header fh {};
            file.read(reinterpret_cast<char*>(&fh), sizeof(struct fat_header));
            // Swap byte order
            if (magic == FAT_MAGIC_64) {
                fh.nfat_arch = OSSwapInt64(fh.nfat_arch);
            } else {
                fh.nfat_arch = OSSwapInt32(fh.nfat_arch);
            }
            // Iterate through all architectures
            for (uint32_t i = 0; i < fh.nfat_arch; i++) {
                MachOInfo machOInfo;
                // Read architecture info
                struct fat_arch fa {};
                file.seekg(sizeof(struct fat_header) + i * sizeof(struct fat_arch));
                file.read(reinterpret_cast<char*>(&fa), sizeof(struct fat_arch));
                // Swap byte order
                if (magic == FAT_MAGIC_64) {
                    fa.cputype = OSSwapInt64(fa.cputype);
                    fa.cpusubtype = OSSwapInt64(fa.cpusubtype);
                    fa.offset = OSSwapInt64(fa.offset);
                    fa.size = OSSwapInt64(fa.size);
                } else {
                    fa.cputype = OSSwapInt32(fa.cputype);
                    fa.cpusubtype = OSSwapInt32(fa.cpusubtype);
                    fa.offset = OSSwapInt32(fa.offset);
                    fa.size = OSSwapInt32(fa.size);
                }
                // Get architecture name
                const NXArchInfo *arch = NXGetArchInfoFromCpuType(fa.cputype, fa.cpusubtype);
                if (!arch) {
                    std::cout << "Unable to get architecture name\n";
                    continue;
                }
                // Print file name and architecture
                //std::cout << "File: " << filename << '\n';
                //std::cout << "Architecture: " << arch->name << '\n';
                machOInfo.arch = arch->name;

                // Navigate to the beginning of architecture
                file.seekg(fa.offset, std::ios::beg);
                // Read the magic number of architecture
                file.read(reinterpret_cast<char*>(&magic), sizeof(uint32_t));

                file.seekg(fa.offset, std::ios::beg);
                // Check for 64-bit
                bool is64Bit = magic == MH_MAGIC_64;

                if (is64Bit) {
                    struct mach_header_64 mh64 {};
                    file.read(reinterpret_cast<char*>(&mh64), sizeof(struct mach_header_64));
                    readAndPrintDependencies(file, mh64, machOInfo);
                } else {
                    struct mach_header mh {};
                    file.read(reinterpret_cast<char*>(&mh), sizeof(struct mach_header));
                    readAndPrintDependencies(file, mh, machOInfo);
                }
                result.emplace_back(std::move(machOInfo));
            }
        } // cases for fat binaries
            break;
        case MH_MAGIC:
        case MH_MAGIC_64:
        case MH_CIGAM:
        case MH_CIGAM_64:
        {
            // Not a fat binary, only one architecture
            cpu_type_t cputype;
            cpu_subtype_t cpusubtype;

            if (magic == MH_MAGIC_64) {
                struct mach_header_64 mh64 {};
                file.read(reinterpret_cast<char*>(&mh64), sizeof(struct mach_header_64));
                cputype = mh64.cputype;
                cpusubtype = mh64.cpusubtype;
            } else {
                struct mach_header mh {};
                file.read(reinterpret_cast<char*>(&mh), sizeof(struct mach_header));
                cputype = mh.cputype;
                cpusubtype = mh.cpusubtype;
            }
            // Get architecture name
            const auto arch = NXGetArchInfoFromCpuType(cputype, cpusubtype);
            if (!arch) {
                std::cout << "Unable to get architecture name\n";
            }
            // Print file name and architecture
            //std::cout << "File: " << filename << '\n';
            //std::cout << "Architecture: " << arch->name << '\n';

            MachOInfo machOInfo;
            machOInfo.arch = arch->name;

            file.seekg(0, std::ios::beg);
            // Check for 64-bit
            bool is64Bit = magic == MH_MAGIC_64;

            if (is64Bit) {
                struct mach_header_64 mh64 {};
                file.read(reinterpret_cast<char*>(&mh64), sizeof(struct mach_header_64));
                readAndPrintDependencies(file, mh64, machOInfo);
            } else {
                struct mach_header mh {};
                file.read(reinterpret_cast<char*>(&mh), sizeof(struct mach_header));
                readAndPrintDependencies(file, mh, machOInfo);
            }
            result.emplace_back(std::move(machOInfo));
        } // cases for thin binaries
            break;
        default:
            std::cout << "File " << filename << " is not a Mach-O file\n";
            return {};
    }
    return result;
}


int main(int argc, char **argv) {
    if (argc < 2) {
        std::cout << "Usage: " << argv[0] << " <mach-o> [<mach-o> ...]\n";
        return 1;
    }
    for (int i = 1; i < argc; i++) {
        auto result = parseMachO(argv[i]);
        std::cout << ANSI_COLOR_BOLD << ANSI_COLOR_BLUE << "- filename: " << ANSI_COLOR_RESET << argv[i] << '\n';
        std::cout << ANSI_COLOR_BOLD << ANSI_COLOR_BLUE << "  info: " << ANSI_COLOR_RESET << '\n';
        for (const auto &item : result) {
            std::cout << ANSI_COLOR_BOLD << ANSI_COLOR_GREEN << "  - arch: " << ANSI_COLOR_RESET << item.arch << '\n';
            std::cout << ANSI_COLOR_BOLD << ANSI_COLOR_GREEN << "    deps: " << ANSI_COLOR_RESET << '\n';
            for (const auto &dep : item.deps) {
                std::cout << "    - " << dep << '\n';
            }
            std::cout << ANSI_COLOR_BOLD << ANSI_COLOR_GREEN << "    rpaths: " << ANSI_COLOR_RESET << '\n';
            for (const auto &rpath : item.rpaths) {
                std::cout << "    - " << rpath << '\n';
            }
        }
        std::cout << '\n';
    }

    return 0;
}
