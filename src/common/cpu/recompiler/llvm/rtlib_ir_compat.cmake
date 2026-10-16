# Rewrites Clang's textual IR so an older llvm-as accepts it. Needed when the
# toolchain Clang is newer than SNX_LLVM_CONFIG's LLVM: its bitcode and its IR
# spellings move faster than that assembler. Only optimizer hints and
# attribute spellings are touched; every instruction survives as written.
#
# usage: cmake -DINPUT=<ir.ll> -DOUTPUT=<ir.ll> -P rtlib_ir_compat.cmake
if(NOT DEFINED INPUT OR NOT DEFINED OUTPUT)
    message(FATAL_ERROR "INPUT and OUTPUT are required")
endif()
file(READ "${INPUT}" ir)
# LLVM 20+ parameter attribute; a newer spelling of "not captured"
string(REGEX REPLACE "captures\\([^)]*\\)[ \t]*" "" ir "${ir}")
# LLVM 18+ icmp flag the older parser rejects before the predicate
string(REGEX REPLACE "samesign[ \t]+" "" ir "${ir}")
# LLVM 20+ attribute
string(REGEX REPLACE "nocreateundeforpoison[ \t]+" "" ir "${ir}")
# The lifetime markers changed signature; they carry no semantics and the
# optimizer recreates whatever it wants from the code's shape
string(REGEX REPLACE "call void @llvm\\.lifetime\\.[^\n]*\n" "" ir "${ir}")
string(REGEX REPLACE "declare void @llvm\\.lifetime\\.[^\n]*\n" "" ir "${ir}")
file(WRITE "${OUTPUT}" "${ir}")
