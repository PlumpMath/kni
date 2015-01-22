#include <fstream>
#include <map>
#include <vector>
#include <sstream>
#include <clang-c/Index.h>
#include <iostream>

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

const std::map<CXTypeKind, std::string>& initializePrimitiveTypesMap() {
    static std::map<CXTypeKind, std::string> m;

    m[CXType_Void] = "V";
    m[CXType_UChar] = "UC";
    m[CXType_UShort] = "US";
    m[CXType_UInt] = "UI";
    m[CXType_ULong] = m[CXType_ULongLong] = "UJ";
    m[CXType_Char_S] = "C";
    m[CXType_SChar] = "Z"; // BOOL in Objective-C
    m[CXType_WChar] = "W";
    m[CXType_Short] = "S";
    m[CXType_Int] = "I";
    m[CXType_Long] = m[CXType_LongLong] = "J";
    m[CXType_Float] = "F";
    m[CXType_Double] = "D";
    // TODO: long double

    m[CXType_ObjCId] = "OI";
    m[CXType_ObjCClass] = "OC";
    m[CXType_ObjCSel] = "OS";

    return m;
}

// TODO: write a long explanation
void serializeType(const CXType& type, std::string& result) {
    // TODO: BlockPointer
    // TODO: enums, structs: are they exposed by libclang?
    // TODO: ConstantArray (in structs?)
    // TODO: Record (for 'va_list' only?)

    // TODO: list of protocols for ObjCInterface type

    static auto primitiveTypes = initializePrimitiveTypesMap();

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
    } else if (type.kind == CXType_Pointer) {
        result += "*";
        auto pointeeType = clang_getPointeeType(type);
        serializeType(pointeeType, result);
        result += ";";
    } else if (type.kind == CXType_ObjCObjectPointer) {
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
    } else {
        // Unsupported kind / unexposed type
        result += "X(";
        AutoCXString spelling = clang_getTypeKindSpelling(type.kind);
        result += spelling.str();
        result += ")";
    }
}

std::string serializeType(const CXType& type) {
    std::string result;
    serializeType(type, result);
    return result;
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
    
    auto category = data->result().add_category();
    // TODO: this name is not unique for nameless categories, either drop them or fix this
    auto name = std::string(categoryDeclInfo->objcClass->name) + "+" + info->entityInfo->name;
    category->set_name(name);

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

    if (info->isDefinition) {
        auto struct_ = data->result().add_struct_();
        struct_->set_name(info->entityInfo->name);
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
        auto type = serializeType(clang_getCursorResultType(info->cursor));
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

void runPostIndexTasks(OutputCollector *data) {
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


std::string *doIndex(const std::vector<std::string>& args) {
    GOOGLE_PROTOBUF_VERIFY_VERSION;

    CXIndex index = clang_createIndex(false, false);
    CXIndexAction action = clang_IndexAction_create(index);

    IndexerCallbacks callbacks = {};
    callbacks.indexDeclaration = indexDeclaration;
    callbacks.diagnostic = diagnostic;

    OutputCollector data;

    std::vector<const char *> cxArgs;
    std::transform(args.begin(), args.end(), std::back_inserter(cxArgs), [](const std::string & s) { return s.c_str(); });

    data.result().set_name(args[0]);

    clang_indexSourceFile(action, &data, &callbacks, sizeof(callbacks), 0, 0,
            &cxArgs[0], static_cast<int>(cxArgs.size()), 0, 0, 0, 0);

    runPostIndexTasks(&data);

    clang_IndexAction_dispose(action);
    clang_disposeIndex(index);

    return data.serialize();
}

void split(const std::string& s, char delimiter, std::vector<std::string>& result) {
    std::stringstream ss(s);
    std::string item;
    while (std::getline(ss, item, delimiter)) {
        result.push_back(item);
    }
}

JNIEXPORT jbyteArray JNICALL Java_org_jetbrains_kni_indexer_IndexerNative_buildNativeIndex
        (JNIEnv *env, jclass, jstring argsString) {
    auto argsChars = env->GetStringUTFChars(argsString, nullptr);
    std::vector<std::string> args;
    split(argsChars, ' ', args);
    env->ReleaseStringUTFChars(argsString, argsChars);

    auto string = doIndex(args);

    auto len = string->length();
    auto result = env->NewByteArray(len);
    env->SetByteArrayRegion(result, 0, len,
            static_cast<const jbyte *>(static_cast<const void *>(string->c_str()))
    );

    delete string;
    
    return result;
}
