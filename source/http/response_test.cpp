// Please note that the content of this file is confidential or protected by law.
// Any unauthorised copying or unauthorised distribution of the information contained herein is prohibited.

#include <boost/test/unit_test.hpp>
#include "http/response.h"
#include "http/test_request_texts.h"

using namespace http;
using namespace http::test;

BOOST_AUTO_TEST_CASE(broken_response_line)
{
    const response_header with_tabs("HTTP/1.1\t100\tI don't know\r\n\r\n");    
    BOOST_CHECK_EQUAL(http::message::syntax_error, with_tabs.state());

    const response_header response_code_out_of_lower_range("HTTP/1.1 99 I don't know\r\n\r\n");    
    BOOST_CHECK_EQUAL(http::message::syntax_error, response_code_out_of_lower_range.state());

    const response_header response_code_out_of_upper_range("HTTP/1.1 600 I don't know\r\n\r\n");    
    BOOST_CHECK_EQUAL(http::message::syntax_error, response_code_out_of_upper_range.state());

    const response_header broken_version("HTTP\\1.1 100 I don't know\r\n\r\n");    
    BOOST_CHECK_EQUAL(http::message::syntax_error, broken_version.state());

    const response_header missing_code("HTTP/4.4  I don't know\r\n\r\n");    
    BOOST_CHECK_EQUAL(http::message::syntax_error, missing_code.state());

    const response_header missing_end_line("HTTP/1.1 100 ok\r\n");
    BOOST_CHECK_EQUAL(http::message::parsing, missing_end_line.state());
}

BOOST_AUTO_TEST_CASE(valid_response_line)
{
    const response_header empty_phrase("HTTP/1.1 222\r\n\r\n");
    BOOST_CHECK_EQUAL(message::ok, empty_phrase.state());
    BOOST_CHECK_EQUAL(1001u, empty_phrase.milli_version());
    BOOST_CHECK_EQUAL(static_cast<http_error_code>(222), empty_phrase.code());

    const response_header smallest_code("HTTp/2.2 100 Ok\r\n\r\n");
    BOOST_CHECK_EQUAL(message::ok, smallest_code.state());
    BOOST_CHECK_EQUAL(2u, smallest_code.major_version());
    BOOST_CHECK_EQUAL(2u, smallest_code.minor_version());
    BOOST_CHECK_EQUAL(http_continue, smallest_code.code());

    const response_header highest_code("http/12.21 599 WTF!\r\n\r\n");
    BOOST_CHECK_EQUAL(message::ok, highest_code.state());
    BOOST_CHECK_EQUAL(12u, highest_code.major_version());
    BOOST_CHECK_EQUAL(21u, highest_code.minor_version());
    BOOST_CHECK_EQUAL(static_cast<http_error_code>(599), highest_code.code());
    BOOST_CHECK_EQUAL("WTF!", highest_code.phrase());
}

/**
 * @test the propper working of the body_expected() function
 */
BOOST_AUTO_TEST_CASE(response_body_expected)
{
    // response is 200 ok, combined with the request method, a body is expected or not
    { 
        const http::response_header header(ok_response_header_apache);

        BOOST_CHECK_EQUAL(message::ok, header.state());
        BOOST_CHECK( header.body_expected(http_options));
        BOOST_CHECK( header.body_expected(http_get));
        BOOST_CHECK(!header.body_expected(http_head));
        BOOST_CHECK( header.body_expected(http_post));
        BOOST_CHECK( header.body_expected(http_put));
        BOOST_CHECK( header.body_expected(http_delete));
        BOOST_CHECK( header.body_expected(http_trace));
        BOOST_CHECK( header.body_expected(http_connect));
    }

    // All 1xx (informational), 204 (no content), and 304 (not modified) responses MUST NOT include a message-body. 
    { 
        const http::response_header info(
            "HTTP/1.1 101 Switching Protocols\r\n\r\n");    

        BOOST_CHECK_EQUAL(message::ok, info.state());
        BOOST_CHECK(!info.body_expected(http_get));

        const http::response_header no_content(
            "HTTP/1.1 204 Nix da\r\n\r\n");    

        BOOST_CHECK_EQUAL(message::ok, no_content.state());
        BOOST_CHECK(!no_content.body_expected(http_get));

        const http::response_header not_modified(cached_response_apache);    

        BOOST_CHECK_EQUAL(message::ok, not_modified.state());
        BOOST_CHECK(!not_modified.body_expected(http_get));
    }

    // All other responses do include a message-body, although it MAY be of zero length. 
    {
        const http::response_header conflict(
            "HTTP/1.0 409 Conflict\r\n\r\n");

        BOOST_CHECK_EQUAL(message::ok, conflict.state());
        BOOST_CHECK(conflict.body_expected(http_delete));

        const http::response_header gateway_timeout(
            "HTTP/1.0 502 GW TO\r\n\r\n");

        BOOST_CHECK_EQUAL(message::ok, gateway_timeout.state());
        BOOST_CHECK(gateway_timeout.body_expected(http_delete));
    }
}
