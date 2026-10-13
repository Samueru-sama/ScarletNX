// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: Copyright 2026 ScarletNX contributors
// Uses unmodified Eden visitors/IR, isolated like tools/maxwell-ir.
#include <bit>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <unordered_map>
#include "maxwell-disas/file_environment.h"
#include "shader_recompiler/frontend/maxwell/decode.h"
#include "shader_recompiler/frontend/maxwell/translate/impl/impl.h"

// Only infrastructure is substituted. Soft assertions become fatal in this
// harness so an assertion fallback cannot be mistaken for a reference result.
void AssertFailSoftImpl() { std::abort(); }
[[noreturn]] void AssertFatalImpl() { std::abort(); }
void Common::Log::FmtLogMessageImpl(Class, Level, const char*, unsigned int, const char*, fmt::string_view format,
                                    const fmt::format_args& args) {
    fmt::print(stderr, "{}\n", fmt::vformat(format, args));
}

namespace IR = Shader::IR;
namespace MW = Shader::Maxwell;

static void translate(MW::TranslatorVisitor& v, u64 word) {
    switch (MW::Decode(word)) {
#define V(name)                                                                                                        \
    case MW::Opcode::name: v.name(word); return;
#define V3(name) V(name##_reg) V(name##_cbuf) V(name##_imm)
        // clang-format off
        V3(MOV) V(MOV32I) V3(I2I) V3(LOP) V(LOP32I) V(P2R_imm)
        V(BFI_reg) V(BFI_rc) V(BFI_cr) V(BFI_imm)
        V(SHF_l_reg) V(SHF_l_imm) V(SHF_r_reg) V(SHF_r_imm)
        V(PSET) V(PSETP) V(CSET) V(CSETP)
        V3(ISET) V3(ISETP) V3(FSET) V3(FSETP) V3(HSETP2) V3(F2I)
#undef V3
#undef V
        default: throw std::runtime_error("instruction outside harness scope");
        // clang-format on
    }
}

static void dump(IR::Block& block) {
    std::unordered_map<const IR::Inst*, size_t> ids;
    size_t id = 0;
    for (auto& inst : block) ids.emplace(&inst, id++);
    fmt::print("[");
    bool first = true;
    for (const auto& inst : block) {
        if (!first) fmt::print(",");
        first = false;
        fmt::print("{{\"op\":\"{}\",\"flags\":{},\"args\":[", IR::NameOf(inst.GetOpcode()), inst.Flags<u32>());
        for (size_t i = 0; i < inst.NumArgs(); ++i) {
            if (i) fmt::print(",");
            const auto arg = inst.Arg(i);
            if (!arg.IsImmediate()) {
                fmt::print("{{\"ref\":{}}}", ids.at(arg.Inst()));
                continue;
            }
            u64 raw;
            switch (arg.Type()) {
                case IR::Type::Reg: raw = static_cast<u64>(arg.Reg()); break;
                case IR::Type::Pred: raw = static_cast<u64>(arg.Pred()); break;
                case IR::Type::U1: raw = arg.U1(); break;
                case IR::Type::U8: raw = arg.U8(); break;
                case IR::Type::U16: raw = arg.U16(); break;
                case IR::Type::U32: raw = arg.U32(); break;
                case IR::Type::U64: raw = arg.U64(); break;
                case IR::Type::F32: raw = std::bit_cast<u32>(arg.F32()); break;
                case IR::Type::F64: raw = std::bit_cast<u64>(arg.F64()); break;
                default: throw std::runtime_error("immediate type outside harness scope");
            }
            fmt::print("{{\"value\":{},\"type\":{}}}", raw, static_cast<u32>(arg.Type()));
        }
        fmt::print("]}}");
    }
    fmt::print("]\n");
}

int main(int argc, char** argv) {
    try {
        const bool decoding = argc > 1 && std::string_view(argv[1]) == "--decode";
        for (int i = decoding ? 2 : 1; i < argc; ++i) {
            char* end;
            const auto word = std::strtoull(argv[i], &end, 16);
            if (*end) return 2;
            if (decoding) {
                fmt::print("{}\n", MW::NameOf(MW::Decode(word)));
                continue;
            }
            FileEnvironment env;
            Shader::ObjectPool<IR::Inst> pool;
            IR::Block block(pool);
            MW::TranslatorVisitor visitor(env, block);
            translate(visitor, word);
            dump(block);
        }
    } catch (const std::exception& e) {
        fmt::print(stderr, "reference rejected: {}\n", e.what());
        return 3;
    }
}
