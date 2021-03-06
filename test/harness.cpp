//-----------------------------------------------------------------------------
// Our harness for running test cases, and reusable checks.
//
// Copyright 2016 whitequark
//-----------------------------------------------------------------------------
#include "harness.h"
#include <regex>
#include <cairo.h>
#if defined(WIN32)
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace SolveSpace {
    // These are defined in headless.cpp, and aren't exposed in solvespace.h.
    extern std::string resourceDir;
    extern std::vector<std::string> fontFiles;
    extern bool antialias;
    extern std::shared_ptr<Pixmap> framebuffer;
}

// The paths in __FILE__ are from the build system, but defined(WIN32) returns
// the value for the host system.
#define BUILD_PATH_SEP (__FILE__[0]=='/' ? '/' : '\\')
#define  HOST_PATH_SEP PATH_SEP

static std::string BuildRoot() {
    static std::string rootDir;
    if(!rootDir.empty()) return rootDir;

    rootDir = __FILE__;
    rootDir.erase(rootDir.rfind(BUILD_PATH_SEP) + 1);
    return rootDir;
}

static std::string HostRoot() {
    static std::string rootDir;
    if(!rootDir.empty()) return rootDir;

    // No especially good way to do this, so let's assume somewhere up from
    // the current directory there's our repository, with CMakeLists.txt, and
    // pivot from there.
#if defined(WIN32)
    wchar_t currentDirW[MAX_PATH];
    GetCurrentDirectoryW(MAX_PATH, currentDirW);
    rootDir = Narrow(currentDirW);
#else
    rootDir = ".";
#endif

    // We're never more than four levels deep.
    for(size_t i = 0; i < 4; i++) {
        std::string listsPath = rootDir;
        listsPath += HOST_PATH_SEP;
        listsPath += "CMakeLists.txt";
        FILE *f = ssfopen(listsPath, "r");
        if(f) {
            fclose(f);
            rootDir += HOST_PATH_SEP;
            rootDir += "test";
            return rootDir;
        }

        if(rootDir[0] == '.') {
            rootDir += HOST_PATH_SEP;
            rootDir += "..";
        } else {
            rootDir.erase(rootDir.rfind(HOST_PATH_SEP));
        }
    }

    ssassert(false, "Couldn't locate repository root");
}

enum class Color {
    Red,
    Green,
    DarkGreen
};

static std::string Colorize(Color color, std::string input) {
#if !defined(WIN32)
    if(isatty(fileno(stdout))) {
        switch(color) {
            case Color::Red:
                return "\e[1;31m" + input + "\e[0m";
            case Color::Green:
                return "\e[1;32m" + input + "\e[0m";
            case Color::DarkGreen:
                return "\e[36m"   + input + "\e[0m";
        }
    }
#endif
    return input;
}

static std::string ReadFile(std::string path) {
    std::string data;
    FILE *f = ssfopen(path.c_str(), "rb");
    if(f) {
        fseek(f, 0, SEEK_END);
        data.resize(ftell(f));
        fseek(f, 0, SEEK_SET);
        fread(&data[0], 1, data.size(), f);
        fclose(f);
    }
    return data;
}

// Normalizes a savefile. Different platforms have slightly different floating-point
// behavior, so if we want to compare savefiles byte-by-byte, we need to do something
// to get rid of irrelevant differences in LSB.
static std::string PrepareSavefile(std::string data) {
    // Round everything to 2**30 ~ 1e9
    const double precision = pow(2, 30);

    size_t newline = 0;
    while(newline < std::string::npos) {
        size_t nextNewline = data.find('\n', newline + 1);

        size_t eqPos = data.find('=', newline + 1);
        if(eqPos < nextNewline) {
            std::string key   = data.substr(newline + 1, eqPos - newline - 1),
                        value = data.substr(eqPos + 1, nextNewline - eqPos - 1);
            for(int i = 0; SolveSpaceUI::SAVED[i].type != 0; i++) {
                if(SolveSpaceUI::SAVED[i].desc != key) continue;
                if(SolveSpaceUI::SAVED[i].fmt  != 'f') continue;
                double f = strtod(value.c_str(), NULL);
                f = round(f * precision) / precision;
                std::string newValue = ssprintf("%.20f", f);
                ssassert(value.size() == newValue.size(), "Expected no change in value length");
                std::copy(newValue.begin(), newValue.end(),
                          data.begin() + eqPos + 1);
            }
        }

        size_t spPos = data.find(' ', newline + 1);
        if(spPos < nextNewline) {
            std::string cmd = data.substr(newline + 1, spPos - newline - 1);
            if(cmd == "Surface" || cmd == "SCtrl" || cmd == "TrimBy"  || cmd == "AddSurface" ||
                cmd == "Curve"  || cmd == "CCtrl" || cmd == "CurvePt" || cmd == "AddCurve") {
                data.replace(newline + 1, nextNewline, nextNewline - newline - 1, ' ');
            }
        }

        newline = nextNewline;
    }
    return data;
}

bool Test::Helper::RecordCheck(bool success) {
    checkCount++;
    if(!success) failCount++;
    return success;
}

void Test::Helper::PrintFailure(const char *file, int line, std::string msg) {
    std::string shortFile = file;
    shortFile.erase(0, BuildRoot().size());
    fprintf(stderr, "test%c%s:%d: FAILED: %s\n",
            BUILD_PATH_SEP, shortFile.c_str(), line, msg.c_str());
}

std::string Test::Helper::GetAssetPath(std::string testFile, std::string assetName,
                                       std::string mangle) {
    if(!mangle.empty()) {
        assetName.insert(assetName.rfind('.'), "." + mangle);
    }
    testFile.erase(0, BuildRoot().size());
    testFile.erase(testFile.rfind(BUILD_PATH_SEP) + 1);
    return PathSepUnixToPlatform(HostRoot() + "/" + testFile + assetName);
}

bool Test::Helper::CheckTrue(const char *file, int line, const char *expr, bool result) {
    if(!RecordCheck(result)) {
        PrintFailure(file, line,
                     ssprintf("(%s) == %s", expr, result ? "true" : "false"));
        return false;
    } else {
        return true;
    }
}

bool Test::Helper::CheckLoad(const char *file, int line, const char *fixture) {
    std::string fixturePath = GetAssetPath(file, fixture);

    FILE *f = ssfopen(fixturePath.c_str(), "rb");
    bool fixtureExists = (f != NULL);
    if(f) fclose(f);

    bool result = fixtureExists &&
        SS.LoadFromFile(fixturePath) && SS.ReloadAllImported(/*canCancel=*/false);
    if(!RecordCheck(result)) {
        PrintFailure(file, line,
                     ssprintf("loading file '%s'", fixturePath.c_str()));
        return false;
    } else {
        SS.AfterNewFile();
        SS.GW.offset = {};
        SS.GW.scale  = 10.0;
        return true;
    }
}

bool Test::Helper::CheckSave(const char *file, int line, const char *reference) {
    std::string refPath = GetAssetPath(file, reference),
                outPath = GetAssetPath(file, reference, "out");
    if(!RecordCheck(SS.SaveToFile(outPath))) {
        PrintFailure(file, line,
                     ssprintf("saving file '%s'", refPath.c_str()));
        return false;
    } else {
        std::string refData = PrepareSavefile(ReadFile(refPath)),
                    outData = PrepareSavefile(ReadFile(outPath));
        if(!RecordCheck(refData == outData)) {
            PrintFailure(file, line, "savefile doesn't match reference");
            return false;
        }

        ssremove(outPath);
        return true;
    }
}

bool Test::Helper::CheckRender(const char *file, int line, const char *reference) {
    PaintGraphics();

    std::string refPath  = GetAssetPath(file, reference),
                outPath  = GetAssetPath(file, reference, "out"),
                diffPath = GetAssetPath(file, reference, "diff");

    std::shared_ptr<Pixmap> refPixmap = Pixmap::ReadPng(refPath.c_str(), /*flip=*/true);
    if(!RecordCheck(refPixmap && refPixmap->Equals(*framebuffer))) {
        framebuffer->WritePng(outPath.c_str(), /*flip=*/true);

        if(!refPixmap) {
            PrintFailure(file, line, "reference render not present");
            return false;
        }

        ssassert(refPixmap->format == framebuffer->format, "Expected buffer formats to match");
        if(refPixmap->width != framebuffer->width ||
           refPixmap->height != framebuffer->height) {
            PrintFailure(file, line, "render doesn't match reference; dimensions differ");
        } else {
            std::shared_ptr<Pixmap> diffPixmap =
                Pixmap::Create(refPixmap->format, refPixmap->width, refPixmap->height);

            int diffPixelCount = 0;
            for(size_t j = 0; j < refPixmap->height; j++) {
                for(size_t i = 0; i < refPixmap->width; i++) {
                    if(!refPixmap->GetPixel(i, j).Equals(framebuffer->GetPixel(i, j))) {
                        diffPixelCount++;
                        diffPixmap->SetPixel(i, j, RgbaColor::From(255, 0, 0, 255));
                    }
                }
            }

            diffPixmap->WritePng(diffPath.c_str(), /*flip=*/true);
            std::string message =
                ssprintf("render doesn't match reference; %d (%.2f%%) pixels differ",
                         diffPixelCount,
                         100.0 * diffPixelCount / (refPixmap->width * refPixmap->height));
            PrintFailure(file, line, message);
        }
        return false;
    } else {
        ssremove(outPath);
        ssremove(diffPath);
        return true;
    }
}

// Avoid global constructors; using a global static vector instead of a local one
// breaks MinGW for some obscure reason.
static std::vector<Test::Case> *testCasesPtr;
int Test::Case::Register(Test::Case testCase) {
    static std::vector<Test::Case> testCases;
    testCases.push_back(testCase);
    testCasesPtr = &testCases;
    return 0;
}

int main(int argc, char **argv) {
#if defined(WIN32)
    _set_abort_behavior(0, _WRITE_ABORT_MSG);
    InitHeaps();
#endif

    std::regex filter(".*");
    if(argc == 1) {
    } else if(argc == 2) {
        filter = argv[1];
    } else {
        fprintf(stderr, "Usage: %s [test filter regex]\n", argv[0]);
        return 1;
    }

    resourceDir = HostRoot();
    resourceDir.erase(resourceDir.rfind(HOST_PATH_SEP) + 1);
    resourceDir += "res";

    fontFiles.push_back(HostRoot() + HOST_PATH_SEP + "Gentium-R.ttf");

    // Different Cairo versions have different antialiasing algorithms.
    antialias = false;

    // Wreck order dependencies between tests!
    std::random_shuffle(testCasesPtr->begin(), testCasesPtr->end());

    auto testStartTime = std::chrono::steady_clock::now();
    size_t ranTally = 0, skippedTally = 0, checkTally = 0, failTally = 0;
    for(Test::Case &testCase : *testCasesPtr) {
        std::string testCaseName = testCase.fileName;
        testCaseName.erase(0, BuildRoot().size());
        testCaseName.erase(testCaseName.rfind(BUILD_PATH_SEP));
        testCaseName += BUILD_PATH_SEP + testCase.caseName;

        std::smatch filterMatch;
        if(!std::regex_search(testCaseName, filterMatch, filter)) {
            skippedTally += 1;
            continue;
        }

        SS.Init();

        Test::Helper helper = {};
        testCase.fn(&helper);

        SK.Clear();
        SS.Clear();

        ranTally   += 1;
        checkTally += helper.checkCount;
        failTally  += helper.failCount;
        if(helper.checkCount == 0) {
            fprintf(stderr, "  %s   test %s (empty)\n",
                    Colorize(Color::Red, "??").c_str(),
                    Colorize(Color::DarkGreen, testCaseName).c_str());
        } else if(helper.failCount > 0) {
            fprintf(stderr, "  %s   test %s\n",
                    Colorize(Color::Red, "NG").c_str(),
                    Colorize(Color::DarkGreen, testCaseName).c_str());
        } else {
            fprintf(stderr, "  %s   test %s\n",
                    Colorize(Color::Green, "OK").c_str(),
                    Colorize(Color::DarkGreen, testCaseName).c_str());
        }
    }

    auto testEndTime = std::chrono::steady_clock::now();
    std::chrono::duration<double> testTime = testEndTime - testStartTime;

    if(failTally > 0) {
        fprintf(stderr, "Failure! %u checks failed\n",
                (unsigned)failTally);
    } else {
        fprintf(stderr, "Success! %u test cases (%u skipped), %u checks, %.3fs\n",
                (unsigned)ranTally, (unsigned)skippedTally,
                (unsigned)checkTally, testTime.count());
    }

    // At last, try to reset all caches we or our dependencies have, to make SNR
    // of memory checking tools like valgrind higher.
    cairo_debug_reset_static_data();

    return (failTally > 0);
}
