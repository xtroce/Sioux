#ifndef SIOUX_SOURCE_PUBSUB_HTTP_CONNECTOR_H
#define SIOUX_SOURCE_PUBSUB_HTTP_CONNECTOR_H

#include <boost/shared_ptr.hpp>
#include <boost/asio/deadline_timer.hpp>
#include "pubsub_http/response.h"
#include "http/request.h"

namespace http {
    class request_header;
}

namespace server {
    class async_response;
}

namespace pubsub
{
    class root;

/**
    @namespace pubsub::http

    # pubsub/http

    Implements a protocol similar to bayeux, but simpler and with observing data, not receiving messages in mind.
    A client subscribes to a versioned data object. The protocol does not guarantie, that the client will see every
    update to the subscribed object.

    ## Protocol

    The server receives http posts with a json encoded message body. The message must be an object with zero or more
    commands and an optional session id. If a session id is not given, the service will respond with a newly generated session id.
    That session id has to be used by the client in every subsequent http post. If the list of commands is empty or missing,
    a session id must be given. The session id value itseld should be treated like an opaque value by a client.

    ### Invalid first message:

        { }

    ### Valid first messages:

        { "id": "ad77df7gb2z7", "cmd": [ { "subscribe": { "a":1 ,"b":2 }, "version": 34 } ] }
        { "cmd": [ { "subscribe": { "a":1 ,"b":2 }, "version": 34 } ] }
        { "id": "ad77df7gb2z7" }

    If a client wants to poll the server, it can so by simply sending it's session id:

        { "id": 123123 }

    The server responses to the client with an object encoded as http response. The object contains two fields,
    one with the session id, one array with the responses to the clients commands.
    For every command send by the client, the server will respond  with a respond. But, the responses might be out of
    order and they dont have to be in the same http response as the commands where. So for example, a https post
    could contain 2 subscriptions. The response to the https post might just contain the respond to the second
    subscription, while a subsequent http post response contains the response to the first subscription.

    ### Possible Command -> Response order:

        client -> { "cmd": [ { "subscribe": { "a": "a1" }, { "subscribe": { "a": "a2" } } ] }
        server <- { "id": 123123, "resp": [ { "subscribe": { "a": "a2" } } ] }
        client -> { "id": 123123 }
        server <- { "id": 123123, "resp": [ { "subscribe": { "a": "a1" }, "error": "no such node" } ] }

    ## The message body

    Every message from the client to the server or from the server to client contains exactly one message. An "id" field
    contains the session id. The session id is always generated by the server. The server will generate a session id,
    if the client doesn't provide a session id, or if the client provided session id is unknown to the server. If the
    server generates a new session id for a client, all subscriptions from the client are void and the client have to
    resubscribe. The server will never generate a session id with the value null.

    Every message from the client to the server can contain a "cmd" field. If the message from the client doens't contain
    a "cmd" field, it must contain an "id" field. The "cmd" field contains an array with 0 or more commands.

    ## Possible Client Messages:

        { "id": 123, "cmd": [] }
        { "id": 123 }
        { "cmd": [ { "subscribe": { "a": 1, "b": 5, "c:" "hallo" } } ] }
        { "id": "abc", "cmd": [ { "subscribe": { "a": 1, "b": 5, "c:" "hallo" } } ] }

    Every message from the server to the client contains a session id. If the received session id, not the session id
    the client received last time, the client must expect that the server was restarted and that all subscriptions are
    void. If the server can not generate a session id, the server will respond with a http error message.

    Every message from the server to the client can optional contain a "resp" (response) -field and an "update" field.
    Both are array. Where the "resp" array elements responses to command send from the client are and the elements in
    the "update" arrays are updates to specific subscribed objects.

    ## Server Message Examples:

        {
            "id": 12,
            "resp": [ { "unsubscribe": {"p1": "a", "p2": "b"} } ],
            "update": [ { "key": { "p1": "a", "p2": "b" }, "data": { "121231" }, "version": 123 } ]
        }
        { "id": {"abc": "def"} }

    ## Subscribe Command

    The subscribe command must contain a "subscribe" field, with an object value. That object denotes the data object
    to subscribe to. Optional, the command can have an additional "version" field. The version field have to contain a
    version of the object and that version have to be obtained from the server during a prior subscription.

    examples:

        client -> { "subscribe": { "market": "bananas", "location": "recife" } }
        client -> { "subscribe": { "a": 1 }, "version": "av34" }

    ## Subscribe Response

    The server will responde with an object, containing the very same "subscribe" value and in case of an error, a
    reason, why the client can not subscribe, in an "error" field.

    examples:

        server -> { "subscribe": { "market": "bananas", "location": "recife" } }
        server -> { "subscribe": { "a": 1 }, "error": "not allowed" }

    ## Unsubscribe Command

    To unsubscribe, the client should send a command with an "unsubscribe" field containing the object key. No other
    fields are expected. A http post response can contain both, update messages to an object and a unsubscribe confirmation.
    In this case, the update has to be ignored, or processed before the unsubscription.

    examples:

        client -> { "unsubscribe": { "market": "bananas", "location": "recife" } }
        client -> { "unsubscribe": { "a": 1 } }

    ## Unsubscribe Response

    The unsubscribe contains the same "unsubscribe" object key value and optional an "error" field, if the client
    can not unsubscribe.

    examples:

        server -> { "unsubscribe": { "market": "bananas", "location": "recife" } }
        server -> { "unsubscribe": { "a": 1 }, "error": "not subscribed" }

    ## updates

    An update is a message that is send only from the server to the client. It contains updates of a the subscribed
    data, or completely new versions of the subscribed data. In both cases, it's an object with a "version" field,
    denoting the new version of the data and "key" field, giving the name of the object. If the message is an update
    from a former version, the message contains a an "update" field and a "from" field. The "update" field contains the
    update operations needed to update the object to the new version and is always an array. "from" is the version
    the updated object must have, so that an update will be successfully. If the clients version of the object is not
    "from", the client should unsubscribe and subscribe _without_ giving a version. If the update contains completely
    new data for an subscribed object, it contains the new data in a "data" field and the new version in a "version" field.
    There could be more than one update for the same key in an http post response from the server. In this case, the
    message have to be read in order.

    examples:

        server -> {
            "key": {  "market": "bananas", "location": "recife" },
            "update": [ 1, 1, 2, "asd" ],
            "from": 123123,
            "version": 123124 }
        server -> { "key": { "a": 1 }, "data": "Hallo", "version": 123 }


 */
namespace http
{
    /**
     * @brief class responsible for creating responses to a pubsub/http endpoint
     */
    template < class Timer = boost::asio::deadline_timer >
    class connector
    {
    public:
        /**
         * @brief creates a connector that connects remote clients to a local pubsub::root instance.
         */
        connector( boost::asio::io_service& queue, pubsub::root& data );

        /**
         * @brief creates a new response object for a given http request.
         *
         * @param connection the connection used to send the response
         * @param header the request
         *
         * @return returns a async_response object, that will communicate the results of the received
         *         requests. If the request was invalid, the function returns a zero pointer and the
         *         caller should create a bad_request response instead.
         */
        template < class Connection >
        boost::shared_ptr< server::async_response > create_response(
            const boost::shared_ptr< Connection >&                    connection,
            const boost::shared_ptr< const ::http::request_header >&  header );

    };

    template < class Timer >
    template < class Connection >
    boost::shared_ptr< server::async_response > connector< Timer >::create_response(
        const boost::shared_ptr< Connection >&                    connection,
        const boost::shared_ptr< const ::http::request_header >&  header )
    {
        if ( header->body_expected() )
            return boost::shared_ptr< server::async_response > ( new response< Connection >( connection ) );

        return boost::shared_ptr< server::async_response >();
    }

}
}

#endif


