#include <iostream>

int run_file_creation_time_tests();
int run_http_request_parser_tests();
int run_http_server_tests();

int wmain() {
    const int timestampResult = run_file_creation_time_tests();
    const int parserResult = run_http_request_parser_tests();
    const int serverResult = run_http_server_tests();

    if (timestampResult != 0 || parserResult != 0 || serverResult != 0) {
        std::cerr << "One or more test suites failed.\n";
        return 1;
    }

    std::cout << "All foobar bridge test suites passed.\n";
    return 0;
}
