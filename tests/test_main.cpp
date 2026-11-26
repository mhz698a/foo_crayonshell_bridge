#include <iostream>

int run_file_creation_time_tests();
int run_http_request_parser_tests();
int run_http_server_tests();
int run_api_route_parser_tests();
int run_timestamp_update_json_tests();
int run_timestamp_job_manager_tests();

int wmain() {
    const int timestampResult = run_file_creation_time_tests();
    const int parserResult = run_http_request_parser_tests();
    const int serverResult = run_http_server_tests();
    const int routeResult = run_api_route_parser_tests();
    const int jsonResult = run_timestamp_update_json_tests();
    const int jobResult = run_timestamp_job_manager_tests();

    if (timestampResult != 0 || parserResult != 0 || serverResult != 0 ||
        routeResult != 0 || jsonResult != 0 || jobResult != 0) {
        std::cerr << "One or more test suites failed.\n";
        return 1;
    }

    std::cout << "All foobar bridge test suites passed.\n";
    return 0;
}
