
#define BOOST_TEST_MAIN
#define BOOST_TEST_MODULE indexer_basics
#include <boost/test/included/unit_test.hpp>

#include "Indexer.hh"
#include "NativeIndex.pb.h"
#include "AutoCXString.h"
#include <boost/filesystem/path.hpp>
#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/fstream.hpp>
#include <boost/range/adaptors.hpp>
#include <boost/range/algorithm.hpp>
#include <memory>

namespace fs = boost::filesystem;

typedef std::vector<std::string> params;

namespace detail {
    class line {
        std::string data;
    public:
        friend std::istream &operator>>(std::istream &is, line &l) {
            std::getline(is, l.data);
            return is;
        }

        operator std::string() const { return data; }

        friend bool operator == (const line& left, const line& right) { return left.data == right.data; }

        friend bool operator != (const line& left, const line& right) { return left.data != right.data; }

        friend std::ostream& operator << (std::ostream &os, const line& l) { return os << l.data; }
    };
}

std::string vector2str(const std::vector<std::string> args)
{
    std::ostringstream os;
    bool first = true;
    os << "{";
    for (auto arg: args) {
        if (!first)
            os << ", \"" << arg << "\"";
        else {
            os << " \"" << arg << "\"";
            first = false;
        }
    }
    os << " }";
    return os.str();
}

#define TEST_LINE_STREAM_EQUALS(istream1, istream2) { \
    typedef std::istream_iterator<detail::line> line_iterator; \
    auto actualBegin = line_iterator(istream1); \
    auto actualEnd = line_iterator(); \
    auto expectedBegin = line_iterator(istream2); \
    auto expectedEnd = line_iterator(); \
    BOOST_CHECK_EQUAL_COLLECTIONS(actualBegin, actualEnd, expectedBegin, expectedEnd); \
}

template <class Cont>
void indexAndCompare(const fs::path& file, Cont args, const fs::path& expectedHint = fs::path()) {

    BOOST_MESSAGE( file.string());

    TranslationUnit tu;
    {
        auto vargs = std::vector<std::string> { file.string() };
        boost::copy(args, std::back_inserter(vargs));
        BOOST_MESSAGE( vector2str(vargs));
        auto message = doIndexToString( vargs);
        tu.ParseFromString(message);
    }

    fs::path expectedFile = file;
    expectedFile.replace_extension("out");
    if (!expectedHint.empty()) {
        if (!fs::exists(expectedHint))
            BOOST_CHECK_MESSAGE(false, "hint path doesn't exist: " << expectedHint);
        else if (fs::is_directory(expectedHint))
            expectedFile = expectedHint / expectedFile.filename();
        else
            expectedFile = expectedHint;
    }

    BOOST_REQUIRE_MESSAGE(fs::exists(expectedFile), "File not found: " << expectedFile);

    {
        fs::path actualFile = expectedFile;
        actualFile.replace_extension("actual");
        fs::ofstream expectedStream(actualFile);
        expectedStream << tu.DebugString();
    }

    std::istringstream actualStream(tu.DebugString());
    fs::ifstream expectedStream(expectedFile);

    TEST_LINE_STREAM_EQUALS(actualStream, expectedStream);
}


BOOST_AUTO_TEST_CASE( indexer_basic)
{
    BOOST_MESSAGE(boost::filesystem::current_path());
    AutoCXString ver(clang_getClangVersion());
    BOOST_MESSAGE("CLang " << ver.str());
    indexAndCompare("testData/c.h", params { });
    indexAndCompare("testData/cpp.hh", params { "-c++" }); // "---v", "---d"
    indexAndCompare("testData/rnd01.hh", params { "-c++" });
    indexAndCompare("../../lib/clang-c/Index.h", params { "-c++" }, "testData/Index.out");
    indexAndCompare("/usr/local/include/wine/windows/windows.h", params { "-c++" }, "testData/windows.out");
    indexAndCompare("testData/objc.h", params { "-ObjC" });
    indexAndCompare("../../tests/testData/integration/simpleClass.h", params { "-ObjC" }, "testData/simpleClass.out");
    indexAndCompare("/System/Library/Frameworks/AppKit.framework/Headers/AppKit.h", params { "-ObjC" }, "testData/AppKit.out");
//    indexAndCompare("/Users/lige/Work/kotlin/kni/tests/testData/integration/arguments/bool.m", params { "-ObjC" });
    //indexAndCompare(boost::filesystem::current_path() / "../../tests/testData/integration_cpp/arguments/PODs.cpp");
}
