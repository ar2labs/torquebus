// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors
//
// Files whose names are not ASCII.
//
// Every string in TorqueBus is UTF-8, and the window hands paths to the core exactly
// as Qt gives them. A narrow path is read by the Windows C++ library in the ANSI code
// page, so a database in "Área de Trabalho" or a project in "Simulação" was reported as
// missing from a folder the person was looking at - by the Lua script loader, the
// database parser, the log files, the model loader - while the window itself, which
// goes through Qt, opened everything without trouble.
//
// The tests build a folder and file names with accents, an arrow and CJK characters,
// and go through each place the core opens a file by name.

#include "core/FilePath.h"
#include "core/log/TraceLog.h"
#include "core/pipeline/NodeCatalog.h"
#include "core/tinyml/TinyMlModel.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <format>
#include <fstream>
#include <random>
#include <string>
#include <vector>

using namespace torquebus;

namespace {

/// A folder with a name no ANSI code page holds all of, removed when the test ends.
class UnicodeFolder final {
public:
    UnicodeFolder()
        : m_path{std::filesystem::temp_directory_path()
                 / pathFromUtf8(std::format("torquebus-caminho-Simulação-ñ-日本-→-{:08x}{:08x}",
                                            std::random_device{}(),
                                            std::random_device{}()))}
    {
        // Named at random: ctest runs each test in a process of its own, several at a time, and
        // a folder they shared would be one test removing the files another is about to open.
        std::filesystem::create_directories(m_path);
    }

    ~UnicodeFolder()
    {
        std::error_code ignored;
        std::filesystem::remove_all(m_path, ignored);
    }

    UnicodeFolder(const UnicodeFolder&) = delete;
    UnicodeFolder& operator=(const UnicodeFolder&) = delete;

    /// The UTF-8 name of a file in the folder, as the window would pass it.
    [[nodiscard]] std::string file(std::string_view name) const
    {
        return utf8FromPath(m_path / pathFromUtf8(name));
    }

    [[nodiscard]] std::string folder() const { return utf8FromPath(m_path); }

private:
    std::filesystem::path m_path;
};

void write(const std::string& utf8Path, const std::string& text)
{
    std::ofstream out{pathFromUtf8(utf8Path), std::ios::binary};
    out << text;
}

} // namespace

TEST(FilePathTests, AUtf8NameComesBackAsTheSameUtf8Name)
{
    for (const std::string name :
         {"plain.txt", "Área de Trabalho", "João/Usuários", "日本語.dbc", "→"}) {
        EXPECT_EQ(utf8FromPath(pathFromUtf8(name)), name);
    }
}

#ifdef _WIN32
TEST(FilePathTests, ThePathTheSystemOpensIsTheOneTheUtf8NamesNotTheAnsiReadingOfItsBytes)
{
    // The native form on Windows is UTF-16, and the conversion is meant to be from UTF-8
    // whatever the machine's ANSI code page is. Independent of the machine: a code page that
    // is 1252 here would turn these bytes into other characters, and one that is already UTF-8
    // cannot tell the two apart - which is why this is spelt out rather than left to the
    // files below to find.
    // The bytes are written out as escapes, so that what is under test is the conversion and
    // not how this file was saved.
    EXPECT_EQ(pathFromUtf8("Simula\xC3\xA7\xC3\xA3o").wstring(), L"Simula\u00E7\u00E3o");
    EXPECT_EQ(pathFromUtf8("\xE6\x97\xA5\xE6\x9C\xAC").wstring(), L"\u65E5\u672C");
}
#endif

TEST(FilePathTests, BytesThatAreNotUtf8DoNotThrow)
{
    // A path somebody pasted from a tool with another encoding. The conversion is a
    // system call that reports a bad sequence by throwing, and this is called from a
    // Start button.
    const std::string broken = "C:\\dados\\\xE3\xE7\xFF.dbc";

    EXPECT_NO_THROW(static_cast<void>(pathFromUtf8(broken)));
    EXPECT_NO_THROW(static_cast<void>(utf8FromPath(pathFromUtf8(broken))));
}

TEST(FilePathTests, ALogRecordedInAFolderWithAccentsIsReadBack)
{
    const UnicodeFolder folder;
    const std::string path = folder.file("gravação-ção.tblog");

    CanFrame frame;
    frame.identifier = 0x321;
    frame.length = 2;
    frame.dlc = 2;
    frame.data[0] = 0xCA;
    frame.data[1] = 0xFE;

    {
        TraceLogWriter writer;
        ASSERT_TRUE(writer.open(path, 1234).succeeded());
        ASSERT_TRUE(writer.append(std::span<const CanFrame>{&frame, 1}).succeeded());
        writer.close();
    }

    TraceLogReader reader;
    ASSERT_TRUE(reader.open(path).succeeded());

    std::vector<CanFrame> frames(4);
    ASSERT_EQ(reader.read(frames), 1U);
    EXPECT_EQ(frames[0].identifier, 0x321U);
    EXPECT_EQ(frames[0].data[1], 0xFE);

    TraceLogSummary summary;
    EXPECT_TRUE(summarize(path, summary).succeeded());
}

TEST(FilePathTests, AScriptNextToAProjectInAFolderWithAccentsIsLoaded)
{
    // The way a project names a script is a path relative to the project's own folder,
    // and the resolution of it goes through the same conversion as the opening.
    const UnicodeFolder folder;
    write(folder.file("motor-ção.lua"), "function on_message(id, data) end\n");

    NodeParameters parameters;
    parameters.set("scriptPath", ParameterValue::fromText("motor-ção.lua"));

    NodeBuildContext context;
    context.basePath = folder.folder();

    const NodeCatalog catalog = NodeCatalog::withBuiltinTypes();
    std::unique_ptr<IPipelineNode> node;
    const Result result = catalog.create("lua.ecu", parameters, context, "ecu", node);

    ASSERT_TRUE(result.succeeded()) << result.message();
    EXPECT_NE(node, nullptr);
}

TEST(FilePathTests, ADatabaseNextToAProjectInAFolderWithAccentsIsLoaded)
{
    const UnicodeFolder folder;
    write(folder.file("rede-ção.dbc"),
          "BO_ 256 Accented: 8 ECU\n SG_ Value : 0|8@1+ (1,0) [0|255] \"\" Vector__XXX\n");

    NodeParameters parameters;
    parameters.set("database", ParameterValue::fromText("rede-ção.dbc"));

    NodeBuildContext context;
    context.basePath = folder.folder();

    const NodeCatalog catalog = NodeCatalog::withBuiltinTypes();
    std::unique_ptr<IPipelineNode> node;
    const Result result = catalog.create("dbc.decoder", parameters, context, "dec", node);

    ASSERT_TRUE(result.succeeded()) << result.message();
    EXPECT_NE(node, nullptr);
}

TEST(FilePathTests, ATinyMlModelInAFolderWithAccentsIsLoaded)
{
    const UnicodeFolder folder;
    const std::string path = folder.file("modelo-ção.tbusml");

    write(path, R"(TBUSML 1
model_name accented
feature_mean 0 0 0 0 0 0
feature_scale 1 1 1 1 1 1
layer head 6 7 linear 0.1
weights 1 0 0 0 0 0
weights 0 1 0 0 0 0
weights 0 0 1 0 0 0
weights 0 0 0 1 0 0
weights 0 0 0 0 1 0
weights 0 0 0 0 0 1
weights 1 1 1 1 1 1
biases 0 0 0 0 0 0 0
)");

    TinyMlModel model;
    const Result result = TinyMlModel::loadFromFile(path, model);

    ASSERT_TRUE(result.succeeded()) << result.message();
    EXPECT_EQ(model.name(), "accented");
}

TEST(FilePathTests, AFileThatIsNotThereIsStillNotThere)
{
    // The conversion must not turn a missing file into a different error, or into a
    // found one.
    const UnicodeFolder folder;

    TraceLogReader reader;
    const Result result = reader.open(folder.file("não-existe.tblog"));

    ASSERT_TRUE(result.failed());
    EXPECT_EQ(result.code(), ErrorCode::FileNotFound);
}
