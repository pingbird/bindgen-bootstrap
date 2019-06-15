#include <iostream>
#include <clang-c/Index.h>
#include <map>
#include <fstream>
#include "json.hpp"

using json = nlohmann::json;

using std::cout;
using std::cerr;
using std::endl;

class ClangString {
public:
    explicit ClangString(CXString string) : _string(string) {}

    ~ClangString() {
        clang_disposeString(_string);
    }

    std::string str() {
        return c_str();
    }

    const char* c_str() {
        return clang_getCString(_string);
    }

private:
    CXString _string;
};

static bool isForwardDecl(CXCursor cursor)  {
    auto definition = clang_getCursorDefinition(cursor);
    if (clang_equalCursors(definition, clang_getNullCursor()))
        return true;
    return !clang_equalCursors(cursor, definition);
}

std::string getTypeSpelling(CXType type) {
    auto ncursor = clang_getTypeDeclaration(type);
    auto nstr = ClangString(clang_getCursorDisplayName(ncursor)).str();
    return nstr.empty() ? ClangString(clang_getTypeSpelling(type)).str() : nstr;
}

std::string getTypedefName(CXType type) {
    ClangString str(clang_getTypedefName(type));
    return str.str();
}

std::string getCursorSpelling(CXCursor cursor) {
    ClangString cursorSpelling(clang_getCursorSpelling(cursor));
    return cursorSpelling.str();
}

static bool isAnonymousType(CXCursor cursor)  {
    if (clang_Cursor_isAnonymous(cursor)) return true;
    auto type = clang_getCursorType(cursor);
    return getTypeSpelling(type).find("::(anonymous") != std::string::npos;
}

int64_t getOffsetOfFieldInBytes(CXCursor cursor) {
    return clang_Cursor_getOffsetOfField(cursor) / 8;
}

std::map<CXTypeKind, const char*> typeKindPrimitives = {
    {CXType_Void, "void"},
    {CXType_Bool, "bool"},

    {CXType_Char_U, "unsigned char"},
    {CXType_UChar, "unsigned char"},
    {CXType_UShort, "unsigned short"},
    {CXType_UInt, "unsigned int"},
    {CXType_ULong, "unsigned long"},
    {CXType_ULongLong, "unsigned long long"},

    {CXType_Char_S, "signed char"},
    {CXType_SChar, "signed char"},
    {CXType_Short, "signed short"},
    {CXType_Int, "signed int"},
    {CXType_Long, "signed long"},
    {CXType_LongLong, "unsigned long long"},

    {CXType_Float, "float"},
    {CXType_Double, "double"},
};

json dumpType(CXType type) {
    if (typeKindPrimitives.count(type.kind) != 0) {
        return {
            {"kind", "Primitive"},
            {"name", typeKindPrimitives[type.kind]}
        };
    } else if (type.kind == CXType_Pointer) {
        return {
            {"kind", "Pointer"},
            {"pointee", dumpType(clang_getPointeeType(type))},
        };
    } else if (type.kind == CXType_FunctionProto) {
        json args = json::array();
        int nArgs = clang_getNumArgTypes(type);
        for (unsigned int i = 0; i < nArgs; i++) {
            args.push_back(dumpType(clang_getArgType(type, i)));
        }

        json out;
        out["kind"] = "Function";
        out["argTypes"] = args;
        out["returnTypes"] = dumpType(clang_getResultType(type));

        if (clang_isFunctionTypeVariadic(type)) {
            out["varadic"] = true;
        }

        return out;
    } else if (type.kind == CXType_Record) {
        CXCursor cursor = clang_getTypeDeclaration(type);
        CXType tpe = clang_getCursorType(cursor);
        return {
            {"kind", "Struct"},
            {"name", getTypeSpelling(tpe)},
        };
    } else if (type.kind == CXType_Enum) {
        return {
            {"kind", "Enum"},
            {"name", getTypeSpelling(type)},
        };
    } else if (type.kind == CXType_ConstantArray) {
        return {
            {"kind", "Array"},
            {"elementType", dumpType(clang_getArrayElementType(type))},
            {"size", clang_getArraySize(type)},
        };
    } else {
        return {{"kind", "Unknown"}, {"id", (unsigned  int)type.kind}, {"name", ClangString(clang_getTypeKindSpelling(type.kind)).str()}};
    }
}

CXChildVisitResult fieldVisitor(CXCursor cursor, CXCursor parent, CXClientData client_data) {
    auto cursorKind = clang_getCursorKind(cursor);
    if (cursorKind == CXCursor_FieldDecl) {
        auto& fields = *reinterpret_cast<json*>(client_data);
        auto type = clang_getCursorType(cursor);
        auto canType = clang_getCanonicalType(type);
        auto size = clang_Type_getSizeOf(type);
        auto offset = getOffsetOfFieldInBytes(cursor);
        auto name = ClangString(clang_getCursorSpelling(cursor)).str();
        auto tpe = dumpType(canType);
        fields.push_back({
            {"size", size},
            {"offset", offset},
            {"name", name},
            {"type", tpe},
        });
    }

    return CXChildVisit_Continue;
}

CXChildVisitResult typeVisitor(CXCursor cursor, CXCursor parent, CXClientData client_data) {
    auto kind = clang_getCursorKind(cursor);

    CXFile file;
    clang_getFileLocation(clang_getCursorLocation(cursor), &file, nullptr, nullptr, nullptr);
    auto fileName = ClangString(clang_getFileName(file)).str();

    if ((kind == CXCursor_ClassDecl || kind == CXCursor_StructDecl) && !isAnonymousType(cursor) && !isForwardDecl(cursor)) {
        auto type = clang_getCursorType(cursor);
        auto size = clang_Type_getSizeOf(type);
        auto name = getTypeSpelling(type);
        auto& info = (*reinterpret_cast<json*>(client_data))["structs"][name];
        info["size"] = size;
        info["fields"] = json::array();
        clang_visitChildren(cursor, fieldVisitor, reinterpret_cast<CXClientData>(&info["fields"]));
        info["fileName"] = fileName;
    } else if (kind == CXCursor_FunctionDecl) {
        auto type = clang_getCursorType(cursor);
        auto canType = clang_getCanonicalType(type);
        auto name = getCursorSpelling(cursor);
        json& info = (*reinterpret_cast<json*>(client_data))["vars"];
        info[name] = dumpType(canType);
        info[name].erase("kind");
        info[name]["fileName"] = fileName;
    } else if (kind == CXCursor_EnumConstantDecl) {
        auto name = getCursorSpelling(cursor);
        auto type = clang_getCanonicalType(clang_getCursorType(cursor));
        json& info = (*reinterpret_cast<json *>(client_data))["constants"][name];
        info["type"] = dumpType(type);
        info["value"] = clang_getEnumConstantDeclValue(cursor);
        info["fileName"] = fileName;
    } else if (kind == CXCursor_VarDecl) {
        auto type = clang_getCursorType(cursor);
        auto canType = clang_getCanonicalType(type);
        auto name = getCursorSpelling(cursor);
        auto eval = clang_Cursor_Evaluate(cursor);
        auto ekind = clang_EvalResult_getKind(eval);
        json outValue;
        bool success = true;
        if (ekind == CXEval_Int) {
            outValue = clang_EvalResult_getAsInt(eval);
        } else if (ekind == CXEval_Float) {
            outValue = clang_EvalResult_getAsDouble(eval);
        } else if (ekind == CXEval_StrLiteral) {
            outValue = clang_EvalResult_getAsStr(eval);
        } else success = false;
        clang_EvalResult_dispose(eval);

        json& info = (*reinterpret_cast<json *>(client_data))["constants"][name];
        if (success) {
            info["fileName"] = fileName;
            info["type"] = dumpType(canType);
            info["value"] = outValue;
        }
    }

    return CXChildVisit_Recurse;
}

int main() {
    CXIndex index = clang_createIndex(0, 0);

    CXTranslationUnit unit;
    auto err = clang_parseTranslationUnit2(
        index,
        "test.h", (const char * const[]){"-I/usr/lib/llvm-6.0/lib/clang/6.0.0/include/", "-I/usr/lib/llvm-6.0/include/"}, 2,
        nullptr, 0,
        CXTranslationUnit_None,
        &unit
    );

    if (err != CXError_Success) {
        if (unit == nullptr) {
            cerr << "Unit null" << endl;
        }

        cerr << "Unable to parse translation unit: " << err << endl;
        exit(-1);
    }

    for (unsigned I = 0, N = clang_getNumDiagnostics(unit); I != N; ++I) {
        CXDiagnostic diag = clang_getDiagnostic(unit, I);
        CXString str = clang_formatDiagnostic(diag, clang_defaultDiagnosticDisplayOptions());
        fprintf(stderr, "%s\n", clang_getCString(str));
        clang_disposeString(str);
    }

    CXCursor rootCursor = clang_getTranslationUnitCursor(unit);

    json out;
    clang_visitChildren(rootCursor, typeVisitor, reinterpret_cast<CXClientData>(&out));

    std::ofstream outfile;
    outfile.open("clang-c.json");
    outfile << out.dump(2) << endl;
    outfile.close();

    cout << out.dump(2) << endl;

    clang_disposeTranslationUnit(unit);
    clang_disposeIndex(index);

    return 0;
}