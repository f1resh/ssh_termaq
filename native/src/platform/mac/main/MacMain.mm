// macOS entry point (SSHTermaq.app/Contents/MacOS/SSHTermaq): the arguments, without Launch Services' -psn_ one, for
// the portable composition root (app/App.h). URLs Launch Services hands over later arrive through the app delegate.
#import <Foundation/Foundation.h>

#include "app/App.h"

#include <string>
#include <vector>

int main(int argc, char** argv) {
    @autoreleasepool {
        std::vector<std::string> args;
        for (int i = 1; i < argc; ++i)
            if (std::string(argv[i]).rfind("-psn_", 0) != 0) args.emplace_back(argv[i]);
        return st::runApp(args);
    }
}
