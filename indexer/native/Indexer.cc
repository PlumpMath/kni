#include <fstream>
#include <map>
#include <vector>
#include <sstream>
#include <iostream>
#include <boost/algorithm/string/trim.hpp>
#include <boost/filesystem/path.hpp>
#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/fstream.hpp>
#include <boost/xpressive/xpressive_static.hpp>

#include "clang-c/Index.h"

#include "asserts.h"
#include "AutoCXString.h"
#include "Indexer.h"
#include "OutputCollector.h"
#include "NativeIndex.pb.h"


std::vector<std::string> extractProtocolNames(const CXIdxObjCProtocolRefListInfo *protocols) {
    std::vector<std::string> result;
    auto numProtocols = protocols->numProtocols;
    for (auto i = 0; i < numProtocols; ++i) {
        auto refInfo = protocols->protocols[i];
        assertNotNull(refInfo);
        auto protocolInfo = refInfo->protocol;
        assertNotNull(protocolInfo);
        result.push_back(protocolInfo->name);
    }
    return result;
}

const CXType untypedefType(CXType type) {
    while (type.kind == CXType_Typedef) {
        auto declaration = clang_getTypeDeclaration(type);
        type = clang_getTypedefDeclUnderlyingType(declaration);
    }
    assertFalse(type.kind == CXType_Invalid);
    return type;
}

// todo: convert static map with non-obvious intitialisation into explicit indexing config/status class
const std::map<CXTypeKind, std::string>& getPrimitiveTypesMap(ProcessingMode::type mode = ProcessingMode::unknown) {
    static std::map<CXTypeKind, std::string> m;

    if (mode != ProcessingMode::unknown) {

        m[CXType_Void] = "V";
        m[CXType_Bool] = "Z";
        m[CXType_Char_U] = "C";
        m[CXType_Char_S] = "C";
        m[CXType_SChar] = mode == ProcessingMode::objc ? "Z" : "B"; // BOOL in Objective-C
        m[CXType_UChar] = "UB";
        m[CXType_WChar] = "W";
        m[CXType_Char16] = "W";
        m[CXType_Char32] = "W";
        m[CXType_Short] = "S";
        m[CXType_UShort] = "US";
        m[CXType_Int] = "I";
        m[CXType_UInt] = "UI";
        m[CXType_Long] = "I";   // assuming that long is at least int, so it is mapped into jvm Integer, for cross-platform compatibility
                                // \todo implement an option for mapping long into jvm Long for some specific cases
        m[CXType_ULong] = "UI";
        m[CXType_LongLong] = "J";
        m[CXType_ULongLong] = "UJ";
        m[CXType_Float] = "F";
        m[CXType_Double] = "D";
        // TODO: long double

        if (mode == ProcessingMode::objc) {
            m[CXType_ObjCId] = "OI";
            m[CXType_ObjCClass] = "OC";
            m[CXType_ObjCSel] = "OS";
        }
    }
    else if (m.empty())
            throw std::logic_error("primitive types map is not initialized");

    return m;
}


std::string getStrippedTypeSpelling(CXType const & type) {
    using namespace boost::xpressive;
    auto ctype = clang_getCanonicalType(type);
    std::string name = AutoCXString( clang_getTypeSpelling(ctype)).str();
    static sregex rex = *(*_s >> (as_xpr("const") | "struct") >> +_s) >> (s1= +_);
    return regex_replace(name, rex, [](smatch const &what){return what[1].str();});
}

// TODO: write a long explanation
void serializeType(const CXType& type, std::string& result) {
    // TODO: BlockPointer
    // TODO: enums, structs: are they exposed by libclang?
    // TODO: ConstantArray (in structs?)
    // TODO: Record (for 'va_list' only?)

    // TODO: list of protocols for ObjCInterface type

    if (type.kind == CXType_Unexposed) {
        auto ctype = clang_getCanonicalType(type);
        if (ctype.kind != CXType_Unexposed) {
            serializeType(ctype, result);
            return;
        }
    }

    if (clang_isConstQualifiedType(type))
        result += "c";

    auto primitiveTypes = getPrimitiveTypesMap();
    auto it = primitiveTypes.find(type.kind);
    if (it != primitiveTypes.end()) {
        result += it->second;
        return;
    }

    auto resultType = clang_getResultType(type);
    if (resultType.kind != CXType_Invalid) {
        result += "(";
        auto numArgs = clang_getNumArgTypes(type);
        for (auto i = 0; i < numArgs; i++) {
            serializeType(clang_getArgType(type, i), result);
        }
        if (clang_isFunctionTypeVariadic(type)) {
            result += ".";
        }
        result += ")";
        serializeType(resultType, result);
        return;
    }

    if (type.kind == CXType_Typedef) {
        serializeType(untypedefType(type), result);
        return;
    }
    if (type.kind == CXType_Pointer) {
        result += "*";
        auto pointeeType = clang_getPointeeType(type);
        serializeType(pointeeType, result);
        result += ";";
        return;
    }
    if (type.kind == CXType_ObjCObjectPointer) {
        auto pointeeType = untypedefType(clang_getPointeeType(type));

        if (pointeeType.kind == CXType_Unexposed) {
            serializeType(pointeeType, result);
        } else if (pointeeType.kind == CXType_ObjCInterface) {
            result += "L";
            auto declaration = clang_getTypeDeclaration(pointeeType);
            assertFalse(clang_isInvalid(declaration.kind));
            AutoCXString spelling = clang_getCursorSpelling(declaration);
            result += spelling.str();
            result += ";";
        } else {
            AutoCXString spelling = clang_getTypeKindSpelling(pointeeType.kind);
            failWithMsg("Unknown Objective-C pointee type: %s\n", spelling.str());
        }
        return;
    }
    if (type.kind == CXType_Record) {
        result += "R";
        result += getStrippedTypeSpelling(type);
        result += ";";
        return;
    }
    // Unsupported kind
    result += "X(";
    AutoCXString spelling = clang_getTypeKindSpelling(type.kind);
    result += spelling.str();
    result += ".";
    AutoCXString tsp = clang_getTypeSpelling(type);
    result += tsp.str();
    result += ")";
}


std::string serializeType(const CXType& type) {
    std::string result;
    serializeType(type, result);
    return result;
}

template <class Target>
void saveLocation(CXIdxDeclInfo const *info, Target *clazz) {
    CXFile file;
    clang_indexLoc_getFileLocation(info->loc, 0, &file, 0, 0, 0);
    auto fname = AutoCXString(clang_getFileName(file));
    if (!fname.empty())
        clazz->set_location_file(fname.str());
}

template <class Target>
void saveContainer(CXIdxDeclInfo const *info, Target *clazz) {
    if (info->semanticContainer) {
        AutoCXString container = clang_getCursorUSR(info->semanticContainer->cursor);
        clazz->set_container(container.str());
    }
}

void indexObjCClass(const CXIdxDeclInfo *info, OutputCollector *data) {
    auto containerDeclInfo = clang_index_getObjCContainerDeclInfo(info);
    assertNotNull(containerDeclInfo);
    if (containerDeclInfo->kind == CXIdxObjCContainer_Implementation) {
        // TODO: report a warning
        return;
    } else if (containerDeclInfo->kind == CXIdxObjCContainer_ForwardRef) {
        data->objc.saveForwardDeclaredClass(info->entityInfo->USR, info->entityInfo->name);
        return;
    }
    assertEquals(containerDeclInfo->kind, CXIdxObjCContainer_Interface);
    assertTrue(info->isDefinition);

    auto interfaceDeclInfo = clang_index_getObjCInterfaceDeclInfo(info);
    assertNotNull(interfaceDeclInfo);

    auto clazz = data->result().add_class_();
    clazz->set_name(info->entityInfo->name);

    auto superInfo = interfaceDeclInfo->superInfo; 
    if (superInfo) {
        auto base = superInfo->base;
        assertNotNull(base);
        clazz->set_base_class(base->name);
    }
    saveContainer(info, clazz);
    saveLocation(info, clazz);

    auto protocols = interfaceDeclInfo->protocols;
    assertNotNull(protocols);
    for (auto protocolName : extractProtocolNames(protocols)) {
        clazz->add_protocol(protocolName);
    }

    data->objc.saveClassByUSR(info->entityInfo->USR, clazz);
}

void indexObjCCategory(const CXIdxDeclInfo *info, OutputCollector *data) {
    assertTrue(info->isDefinition);
    auto categoryDeclInfo = clang_index_getObjCCategoryDeclInfo(info);
    assertNotNull(categoryDeclInfo);

    // note: the categories with empty category name (entityInfo->name) is are not unique and should be treated
    // accordingly on generation (in most cases - just ignored)
    auto category = data->result().add_category();
    auto name = std::string(categoryDeclInfo->objcClass->name) + "+" + info->entityInfo->name;
    category->set_name(name);
    saveContainer(info, category);
    saveLocation(info, category);

    auto clazz = data->objc.loadClassByUSR(categoryDeclInfo->objcClass->USR);
    assertNotNull(clazz);
    clazz->add_category(name);

    auto protocols = categoryDeclInfo->protocols;
    assertNotNull(protocols);
    for (auto protocolName : extractProtocolNames(protocols)) {
        category->add_base_protocol(protocolName);
    }

    data->objc.saveCategoryByUSR(info->entityInfo->USR, category);
}

void indexObjCProtocol(const CXIdxDeclInfo *info, OutputCollector *data) {
    auto containerDeclInfo = clang_index_getObjCContainerDeclInfo(info);
    assertNotNull(containerDeclInfo);
    if (containerDeclInfo->kind == CXIdxObjCContainer_ForwardRef) {
        data->objc.saveForwardDeclaredProtocol(info->entityInfo->USR, info->entityInfo->name);
        return;
    }
    assertEquals(containerDeclInfo->kind, CXIdxObjCContainer_Interface);
    assertTrue(info->isDefinition);

    auto protocol = data->result().add_protocol();
    protocol->set_name(info->entityInfo->name);
    saveContainer(info, protocol);
    saveLocation(info, protocol);

    auto protocols = clang_index_getObjCProtocolRefListInfo(info);
    assertNotNull(protocols);
    for (auto protocolName : extractProtocolNames(protocols)) {
        protocol->add_base_protocol(protocolName);
    }

    data->objc.saveProtocolByUSR(info->entityInfo->USR, protocol);
}

std::string getNotNullSemanticContainerUSR(const CXIdxDeclInfo *info) {
    assertNotNull(info->semanticContainer);
    AutoCXString container = clang_getCursorUSR(info->semanticContainer->cursor);
    return container.str();
}

ObjCMethod *createMethodInItsContainer(const CXIdxDeclInfo *info, OutputCollector *data) {
    auto container = getNotNullSemanticContainerUSR(info);

    auto clazz = data->objc.loadClassByUSR(container);
    if (clazz) return clazz->add_method();
    auto protocol = data->objc.loadProtocolByUSR(container);
    if (protocol) return protocol->add_method();
    auto category = data->objc.loadCategoryByUSR(container);
    if (category) return category->add_method();

    return nullptr;
}

void constructFunction(CXIdxDeclInfo const *info, Function *function) {
    function->set_name(info->entityInfo->name);

    auto type = serializeType(clang_getCursorResultType(info->cursor));
    function->set_return_type(type);

    // TODO: handle variadic arguments
    auto numArguments = clang_Cursor_getNumArguments(info->cursor);
    for (auto i = 0; i < numArguments; ++i) {
        auto argument = clang_Cursor_getArgument(info->cursor, i);
        auto parameter = function->add_parameter();
        AutoCXString name = clang_getCursorSpelling(argument);
        parameter->set_name(name.str());
        auto type = serializeType(clang_getCursorType(argument));
        parameter->set_type(type);
    }
}


void indexMethod(const CXIdxDeclInfo *info, OutputCollector *data, bool isClassMethod) {
    ObjCMethod *method = createMethodInItsContainer(info, data);
    assertNotNull(method);

    method->set_class_method(isClassMethod);

    constructFunction(info, method->mutable_function());
}

void indexFunction(const CXIdxDeclInfo *info, OutputCollector *data) {
    auto container = getNotNullSemanticContainerUSR(info);
    if (!data->objc.anyCategoryByUSR(container))
        constructFunction(info, data->result().add_function());
}

void indexStruct(const CXIdxDeclInfo *info, OutputCollector *data) {

    std::string name;
    if (info->entityInfo->name)
        name.assign(info->entityInfo->name);
    else {
        auto cur = info->entityInfo->cursor;
        auto type = clang_getCursorType(cur);
        name.assign( AutoCXString(clang_getTypeSpelling(type)).str());
    }
    if (info->isDefinition) {
        auto struct_ = data->result().add_struct_();
        struct_->set_name(name);
        data->c.structs[info->entityInfo->USR] = struct_;
    }
    else
        data->c.forwardStructs.insert(std::make_pair(info->entityInfo->USR, info->entityInfo->name));
}

void indexField(const CXIdxDeclInfo *info, OutputCollector *data) {
    auto container = getNotNullSemanticContainerUSR(info);
    auto structIt = data->c.structs.find(container);
    if (structIt != data->c.structs.end()) {
        auto field = structIt->second->add_field();
        field->set_name(info->entityInfo->name);
        auto type = serializeType(clang_getCursorType(info->cursor));
        field->set_type(type);
    }
}

ObjCProperty *createPropertyInItsContainer(const CXIdxDeclInfo *info, OutputCollector *data) {
    // TODO: generify the code somehow (see the same method above)
    auto container = getNotNullSemanticContainerUSR(info);

    auto clazz = data->objc.loadClassByUSR(container);
    if (clazz) return clazz->add_property();
    auto protocol = data->objc.loadProtocolByUSR(container);
    if (protocol) return protocol->add_property();
    auto category = data->objc.loadCategoryByUSR(container);
    if (category) return category->add_property();

    return nullptr;
}

void indexProperty(const CXIdxDeclInfo *info, OutputCollector *data) {
    ObjCProperty *property = createPropertyInItsContainer(info, data);
    assertNotNull(property);

    property->set_name(info->entityInfo->name);

    auto type = serializeType(clang_getCursorType(info->cursor));
    property->set_type(type);
}

void indexDeclaration(CXClientData clientData, const CXIdxDeclInfo *info) {
    assertNotNull(clientData);
    assertNotNull(info);
    assertNotNull(info->entityInfo);

    OutputCollector *data = static_cast<OutputCollector *>(clientData);

    if (data->mode() == ProcessingMode::objc)
        switch (info->entityInfo->kind) {
            case CXIdxEntity_ObjCClass:
                indexObjCClass(info, data); break;
            case CXIdxEntity_ObjCProtocol:
                indexObjCProtocol(info, data); break;
            case CXIdxEntity_ObjCCategory:
                indexObjCCategory(info, data); break;
            case CXIdxEntity_ObjCInstanceMethod:
                indexMethod(info, data, false); break;
            case CXIdxEntity_ObjCClassMethod:
                indexMethod(info, data, true); break;
            case CXIdxEntity_ObjCProperty:
                indexProperty(info, data); break;
            default:
                break;
        }
    else
        switch (info->entityInfo->kind) {
            case CXIdxEntity_Function:
                indexFunction(info, data); break;
            case CXIdxEntity_Struct:
                indexStruct(info, data); break;
            case CXIdxEntity_Field:
                indexField(info, data); break;
            default:
                break;
        }
}

void diagnostic(CXClientData clientData, CXDiagnosticSet diagSet, void *reserved) {
    OutputCollector *data = static_cast<OutputCollector *>(clientData);
    unsigned count = clang_getNumDiagnosticsInSet(diagSet);
    if (count > 0) {
        auto diag = data->result().add_diagnostic();
        for (unsigned i = 0; i < count; i++) {
            std::shared_ptr<void> cxDiag(clang_getDiagnosticInSet(diagSet, i), clang_disposeDiagnostic);

            diag->set_severity( clang_getDiagnosticSeverity(cxDiag.get()));

            CXSourceLocation location = clang_getDiagnosticLocation(cxDiag.get());
            unsigned line, column;
            clang_getExpansionLocation(location, 0, &line, &column, 0);
            diag->set_line(line);
            diag->set_column(column);

            diag->set_message( AutoCXString( clang_formatDiagnostic(cxDiag.get(), clang_defaultDiagnosticDisplayOptions())).str());
            diag->set_category( AutoCXString( clang_getDiagnosticCategoryText(cxDiag.get())).str());
        }
    }
}

void runPostIndexTasks(const std::shared_ptr<OutputCollector> & data) {
    // For every forward-declared @class or @protocol which was never defined,
    // we create an empty class or protocol here. This is needed because a
    // pointer to such a class can still appear in the type position of method
    // arguments or return type of a method, regardless of whether or not
    // it was defined

    auto classes = data->objc.loadForwardDeclaredClasses();
    for (auto clazz : classes) {
        auto usr = clazz.first;
        if (data->objc.loadClassByUSR(usr)) continue;

        auto name = clazz.second;
        auto newClass = data->result().add_class_();
        newClass->set_name(name);
    }

    auto protocols = data->objc.loadForwardDeclaredProtocols();
    for (auto protocol : protocols) {
        auto usr = protocol.first;
        if (data->objc.loadProtocolByUSR(usr)) continue;

        auto name = protocol.second;
        auto newProtocol = data->result().add_protocol();
        newProtocol->set_name(name);
    }
}


std::shared_ptr<OutputCollector> doIndex(const std::vector<std::string>& args) {
    GOOGLE_PROTOBUF_VERIFY_VERSION;

    namespace fs=boost::filesystem;

    std::vector<const char *> cxArgs;
    fs::path name;
    bool verbose = false;
    bool debugDump = false;
    boost::filesystem::path dumpPath;
    ProcessingMode::type mode = ProcessingMode::unknown;
    // \todo consider more advanced parsing
    for (auto const& arg: args) {
        // checking for args to indexer, that should be filtered out
        if (arg.substr(0, 4) == "---d") {
            debugDump = true;
            using namespace boost::xpressive;
            static sregex dump_rex = as_xpr("---d=") >> (s1 = *_);
            smatch what;
            if (regex_match(arg, what, dump_rex)) {
                dumpPath = fs::path(what[1]);
                if (!fs::exists(dumpPath)) {
                    std::cerr << "Error: dump target path '" << dumpPath << "' doesn't exist " << std::endl;
                    dumpPath.clear();
                }
            }
        }
        else if (arg == "---v") verbose = true;
        else {
            // other argumens are passed to libclang
            if (arg[0] != '-') {
                assertTrue(name.empty());
                name = arg;
            }
            else {
                cxArgs.push_back(arg.c_str());
                if (arg == "-ObjC") {
                    assertTrue(mode == ProcessingMode::unknown);
                    mode = ProcessingMode::objc;
                }
            }
        }
    }
    if (mode == ProcessingMode::unknown)
        mode = ProcessingMode::cpp;

    if (verbose) {
        std::cerr << "Indexing in " << ProcessingMode::str(mode) << " mode '" << name << "' with args:";
        for (auto const& arg: cxArgs) std::cerr << " " << arg;
        std::cerr << std::endl;
    }
    auto data = std::shared_ptr<OutputCollector>(new OutputCollector(mode));

    data->result().set_name(name/*.filename()*/.string());
    
    getPrimitiveTypesMap(mode);

    std::shared_ptr<void> index(clang_createIndex(false, false), clang_disposeIndex);
    std::shared_ptr<void> action(clang_IndexAction_create(index.get()), clang_IndexAction_dispose);

    IndexerCallbacks callbacks = {};
    callbacks.indexDeclaration = indexDeclaration;
    callbacks.diagnostic = diagnostic;

    clang_indexSourceFile(action.get(), data.get(), &callbacks, sizeof(callbacks), 0, name.c_str(),
            &cxArgs[0], static_cast<int>(cxArgs.size()), 0, 0, 0, 0);

    runPostIndexTasks(data);

    if (debugDump) {
        if (dumpPath.empty()) dumpPath = name.parent_path();
        if (fs::is_directory(dumpPath)) dumpPath /= name.filename().replace_extension("dump");
        if (verbose)
            std::cerr << "Dumping parsing results to '" << dumpPath << "'" << std::endl;
        fs::ofstream os(dumpPath);
        os << data->debugString() << std::endl;
        os.close();
    }

    return data;
}

std::string doIndexToString(const std::vector<std::string>& args) {
    return doIndex(args)->serialize();
}

void split(const std::string& s, char delimiter, std::vector<std::string>& result) {
    std::stringstream ss(s);
    std::string item;
    while (std::getline(ss, item, delimiter)) {
        result.push_back(item);
    }
}

JNIEXPORT jbyteArray JNICALL Java_org_jetbrains_kni_indexer_IndexerNative_buildNativeIndex
        (JNIEnv *env, jclass, jobjectArray stringArray) {

    int stringCount = env->GetArrayLength(stringArray);

    std::vector<std::string> args;
    for (int i=0; i<stringCount; i++) {
        jstring string = (jstring) env->GetObjectArrayElement(stringArray, i);
        const char *rawString = env->GetStringUTFChars(string, 0);
        args.push_back(rawString);
        env->ReleaseStringUTFChars(string, rawString);
    }

    auto data = doIndex(args);

    auto len = data->serializedSize();
    auto result = env->NewByteArray(len);
    jboolean isCopy;
    jbyte* rawjBytes = env->GetByteArrayElements(result, &isCopy);
    data->serializeToArray(rawjBytes, len);
    env->ReleaseByteArrayElements(result, rawjBytes, 0);

//    env->SetByteArrayRegion(result, 0, len,
//            static_cast<const jbyte *>(static_cast<const void *>(data->serialize().c_str()))
//    );

    return result;
}
