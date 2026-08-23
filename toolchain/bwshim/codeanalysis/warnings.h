// Stub for Visual Studio's <codeanalysis/warnings.h>.
//
// BoxedWine includes this under BOXEDWINE_MSVC, and we need BOXEDWINE_MSVC for
// its Windows platform defines (OPCALL __fastcall, the _stat32i64 typedefs, the
// unistd/direct.h mappings). But the header ships with Visual Studio itself,
// not with the Windows SDK, and toolchain/xwin-sdk is the SDK alone.
//
// Nothing we compile reads anything out of it: the only symbol BoxedWine takes
// from it is ALL_CODE_ANALYSIS_WARNINGS, which platformBoxedwine.h defines for
// itself, and the three files that use it are UI/utility code outside the JIT
// cut. So an empty header is the whole of it, and it keeps upstream unpatched.
