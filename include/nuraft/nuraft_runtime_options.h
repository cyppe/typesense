#pragma once

#include <cmdline.h>

#include <string>

#include "nuraft/nuraft_http_runtime.h"
#include "tsconfig.h"

void init_nuraft_runtime_cmdline_options(cmdline::parser& options, int argc, char** argv);

bool load_nuraft_runtime_options(cmdline::parser& options,
                                 int argc,
                                 char** argv,
                                 Config& config,
                                 NuRaftHttpServerOptions& runtime_options,
                                 bool& help_requested,
                                 std::string& usage,
                                 std::string& error);
