#include <iostream>
#include <cstdlib>

#include "dbrecovery/tools/recovery_cli.hpp"

int main(int argc, char** argv) {
    dbrecovery::tools::RecoveryCLI cli(argc, argv);
    return cli.run(argc, argv);
}