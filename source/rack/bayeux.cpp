// Copyright (c) Torrox GmbH & Co KG. All rights reserved.
// Please note that the content of this file is confidential or protected by law.
// Any unauthorised copying or unauthorised distribution of the information contained herein is prohibited.


#include <ruby.h>
#include <assert.h>
#include <iostream>
#include <boost/bind.hpp>
#include <boost/utility.hpp>
#include <boost/thread/future.hpp>

#include "rack/response.h"
#include "rack/response.inc"
#include "rack/ruby_tools.h"
#include "rack/adapter.h"
#include "rack/log.h"
#include "server/server.h"
#include "server/secure_session_generator.h"
#include "bayeux/bayeux.h"
#include "bayeux/log.h"
#include "bayeux/node_channel.h"
#include "pubsub/pubsub.h"
#include "pubsub/configuration.h"
#include "tools/split.h"
#include "tools/iterators.h"
#include "tools/log.h"
#include "tools/exception_handler.h"

/* Design:
 * - For easier testing the server should only bind to it's listen-ports, when the function Rack::Handler::Sioux.run()
 *   runs. This implies that the server is created locally at the stack of that function.
 * - All ruby objects passed to Rack::Handler::Sioux.run() must be marked as referenced from the outside, so that there
 *   is no need to additional mark them during the GC mark phase.
 * - Notification callbacks must be executed from a ruby thread context. For reacquiring the global vm lock (gvl) there
 *   is currently no API function. So the C++ server run's in it's own thread, while the ruby threads wait for callback
 *   function to be executed.
 * - Calls to the ruby API can not be made from the native, not-ruby threads.
 */

namespace
{
    using namespace rack;

    typedef
#       ifdef NDEBUG
            server::logging_server<
                server::null_event_logger,
                server::null_error_logger >
#       else
            server::logging_server<
                bayeux::stream_event_log< server::stream_event_log > >
#       endif
            server_t;

    typedef server_t::connection_t connection_t;
    typedef rack::response< connection_t > response_t;

    class bayeux_server :
        private bayeux::adapter< VALUE >,
        private rack::application_interface,
        private boost::noncopyable
    {
    public:
        bayeux_server( VALUE application, VALUE ruby_self, VALUE configuration );

        ~bayeux_server();

        void run();

        void subscribe_test( const pubsub::node_name& );

        /**
         * This functions marks all references to ruby objects that are stored by the server
         */
        void mark_ruby_references();

        /**
         * @brief update the given node to the given value
         */
        void update_node(const pubsub::node_name& node_name, const json::value& new_data);

    private:
        boost::shared_ptr< server::async_response > on_bayeux_request(
                    const boost::shared_ptr< connection_t >&                connection,
                    const boost::shared_ptr< const http::request_header >&  request );

        boost::shared_ptr< server::async_response > on_request(
                    const boost::shared_ptr< connection_t >&                connection,
                    const boost::shared_ptr< const http::request_header >&  request );

        static pubsub::configuration pubsub_config( VALUE configuration );

        // bayeux::adapter implementation
        std::pair< bool, json::string > handshake( const json::value& ext, VALUE& session );
        std::pair< bool, json::string > publish( const json::string& channel, const json::value& data,
            const json::object& message, VALUE& session, pubsub::root& root );

        // rack::application_interface implementation
        std::vector< char > call( const std::vector< char >& body, const http::request_header& request );

        // calls an optional configurated call back: configuration['Adapter'].init(self)
        // it's important, that
        void call_init_hook();

        // run the C++-land io queue
        void run_queue();

        // a pointer is used to destroy the queue_ and thus the contained response-objects before the server and
        // thus accessed objects like the logging-trait
        boost::asio::io_service*            queue_;
        const VALUE                         app_;
        const VALUE                         self_;
        const VALUE                         configuration_;
        const VALUE                         ruby_adapter_;
        rack::ruby_land_queue               ruby_land_queue_;
        rack::adapter                       adapter_;
        pubsub::root                        root_;
        server::secure_session_generator    session_generator_;
        bayeux::connector<>                 connector_;
        server_t                            server_;
    };

    bayeux_server::bayeux_server( VALUE application, VALUE ruby_self, VALUE configuration )
        : queue_( new boost::asio::io_service )
        , app_( application )
        , self_( ruby_self )
        , configuration_( configuration )
        , ruby_adapter_( rb_hash_lookup( configuration_, rb_str_new2( "Adapter" ) ) )
        , adapter_( ruby_adapter_, ruby_land_queue_ )
        , root_( *queue_, adapter_, pubsub_config( configuration ) )
        , connector_( *queue_, root_, session_generator_, *this, bayeux::configuration() )
        , server_( *queue_, 0, std::cout )
    {
        logging::add_output( std::cout );

        LOG_INFO( rack::log_context << "starting bayeux_server...." );

        server_.add_action( "/bayeux", boost::bind( &bayeux_server::on_bayeux_request, this, _1, _2 ) );
        server_.add_action( "/", boost::bind( &bayeux_server::on_request, this, _1, _2 ) );

        const unsigned port = rack::from_hash( configuration, "Port" );

        using namespace boost::asio::ip;
        server_.add_listener( tcp::endpoint( address( address_v4::any() ), port ) );
//        server_.add_listener( tcp::endpoint( address( address_v6::any() ), port ) );
    }

    bayeux_server::~bayeux_server()
    {
        delete queue_;
    }

    void bayeux_server::call_init_hook()
    {
        VALUE adapter = rb_hash_lookup( configuration_, rb_str_new2( "Adapter" ) );

        if ( adapter != Qnil )
        {
            static const ID call = rb_intern( "init" );

            if ( rb_respond_to( adapter, call ) )
                rb_funcall( adapter, call, 1, self_ );
        }
    }

    void bayeux_server::run_queue()
    {
        for ( ;; )
        {
            try
            {
                queue_->run();
                break;
            }
            catch ( ... )
            {
                LOG_ERROR( rack::log_context << "in bayeux_server::run_queue(): "  << tools::exception_text() );
            }
        }
    }

    typedef std::pair< boost::thread*, server_t* > join_data_t;

    extern "C" VALUE bayeux_join_threads( void* arg )
    {
        join_data_t *const threads = static_cast< join_data_t* >( arg );
        threads->first->join();
        threads->second->join();

        return VALUE();
    }

    extern "C" void bayeux_stop_joining_threads( void* arg )
    {
        boost::asio::io_service *const queue = static_cast< boost::asio::io_service* >( arg );

        queue->stop();
    }

    void bayeux_server::run()
    {
        call_init_hook();
        boost::thread queue_runner( boost::bind( &bayeux_server::run_queue, this ) );

        ruby_land_queue_.process_request( *this );
        server_.shut_down();
        connector_.shut_down();

        join_data_t joindata( &queue_runner, &server_ );
        rb_thread_blocking_region( &bayeux_join_threads, &joindata, &bayeux_stop_joining_threads, &queue_ );
    }


    void bayeux_server::subscribe_test( const pubsub::node_name& name )
    {
        class subs_t : public pubsub::subscriber
        {
            void on_update(const pubsub::node_name& name, const pubsub::node& data) {}
        };

        root_.subscribe(
            boost::shared_ptr< pubsub::subscriber >( static_cast< pubsub::subscriber* >( new subs_t ) ),
            name );
    }

    void bayeux_server::mark_ruby_references()
    {
    }

    void bayeux_server::update_node(const pubsub::node_name& node_name, const json::value& new_data)
    {
        root_.update_node( node_name, new_data );
    }

    boost::shared_ptr< server::async_response > bayeux_server::on_bayeux_request(
                const boost::shared_ptr< connection_t >&                connection,
                const boost::shared_ptr< const http::request_header >&  request )
    {
        return connector_.create_response( connection, request );
    }

    boost::shared_ptr< server::async_response > bayeux_server::on_request(
                const boost::shared_ptr< connection_t >&                connection,
                const boost::shared_ptr< const http::request_header >&  request )
    {
        const boost::shared_ptr< server::async_response > result(
            new rack::response< connection_t >( connection, request, *queue_, ruby_land_queue_ ) );

        return result;
    }

    pubsub::configuration bayeux_server::pubsub_config( VALUE /* configuration */ )
    {
        return pubsub::configuration();
    }

    std::pair< bool, json::string > bayeux_server::handshake( const json::value& /* ext */, VALUE& session )
    {
        session = Qnil;

        return std::pair< bool, json::string >( true, json::string() );
    }

    typedef std::pair< bool, json::string > publish_result_t;

    static publish_result_t convert_call_back_result( VALUE answer, const pubsub::node_name& node,
        const char* error_context_msg )
    {
        if ( TYPE( answer ) != T_ARRAY )
        {
            LOG_ERROR( log_context << error_context_msg << node << "\" => " << " answer is not a ruby array" );
        }
        else if ( RARRAY_LEN( answer ) != 2 )
        {
            LOG_ERROR( log_context << error_context_msg << node << "\" => " << " size of received array is not 2" );
        }
        else
        {
            const VALUE first_arg  = RARRAY_PTR( answer )[ 0 ];
            const VALUE second_arg = RARRAY_PTR( answer )[ 1 ];
            const VALUE error_message = NIL_P( second_arg ) ? second_arg : rb_check_string_type( second_arg );

            if ( NIL_P( second_arg ) && !NIL_P( error_message ) )
            {
                LOG_ERROR( log_context << error_context_msg << node << "\" => " << " unable to convert second argument to String." );
            }
            else
            {
                return publish_result_t( RTEST( first_arg ), rack::rb_str_to_json( error_message ) );
            }
        }

        return publish_result_t( false, json::string( "internal error" ) );

    }

    static void bayeux_publish_impl( boost::promise< publish_result_t >& result,
        const pubsub::node_name& node, const json::value& data, const json::object& message, VALUE root, VALUE adapter,
        rack::application_interface& )
    {
        static const ID publish_function = rb_intern( "publish" );
        static const char* error_context_msg = "while trying to upcall bayeux publish handler for node: \"";

        try
        {
            if ( !rb_respond_to( adapter, publish_function ) )
            {
                result.set_value( publish_result_t( false, json::string( "no callback installed." ) ) );
                return;
            }

            const VALUE ruby_node  = rack::node_to_hash( node );
            const VALUE ruby_value = rack::json_to_ruby( data );

            const VALUE answer = rb_funcall( adapter, publish_function, 3, ruby_node, ruby_value, root );
            result.set_value( convert_call_back_result( answer, node, error_context_msg ) );
        }
        catch ( ... )
        {
            LOG_ERROR( log_context << error_context_msg << node << "\" => "
                << tools::exception_text() );

            // attention!, the error text is communicated to the outside.
            result.set_value( publish_result_t( false, json::string( "internal error" ) ) );
        }
    }

    std::pair< bool, json::string > bayeux_server::publish( const json::string& channel, const json::value& data,
        const json::object& message, VALUE& session, pubsub::root& root )
    {
        boost::promise< publish_result_t > result;

        const pubsub::node_name node = bayeux::node_name_from_channel( channel );

        rack::ruby_land_queue::call_back_t ruby_execution(
            boost::bind( bayeux_publish_impl, boost::ref( result ), node, data, message, self_, ruby_adapter_, _1 ) );

        ruby_land_queue_.push( ruby_execution );
        return result.get_future().get();
    }

    static void fill_http_headers( VALUE environment, const http::request_header& request )
    {
        for ( http::request_header::const_iterator header = request.begin(), end = request.end(); header != end; ++header )
        {
            static const ID upcase = rb_intern( "upcase!" );

            VALUE header_name = rb_str_concat( rb_str_new2( "HTTP_" ), rack::rb_str_new_sub( header->name() ) );
            header_name = rb_funcall( header_name, upcase, 0 );

            rb_hash_aset( environment, header_name, rack::rb_str_new_sub( header->value() ) );
        }
    }

    static void fill_header( VALUE environment, const http::request_header& request )
    {
        using namespace rack;

        rb_hash_aset( environment, rb_str_new2( "REQUEST_METHOD" ), rb_str_new2( tools::as_string( request.method() ).c_str() ) );

        tools::substring scheme, authority, path, query, fragment;
        http::split_url( request.uri(), scheme, authority, path, query, fragment );

        // SCRIPT_NAME + PATH_INFO should yield path, where PATH_INFO is the 'mounting point'
        // we don't have a mounting point, thus SCRIPT_NAME = empty and PATH_INFO = path
        rb_hash_aset( environment, rb_str_new2( "SCRIPT_NAME" ), rb_str_new( "", 0 ) );
        rb_hash_aset( environment, rb_str_new2( "PATH_INFO"), rb_str_new_sub( path ) );
        rb_hash_aset( environment, rb_str_new2( "QUERY_STRING" ), rb_str_new_sub( query ) );

        rb_hash_aset( environment, rb_str_new2( "SERVER_NAME" ), rb_str_new_sub( request.host() ) );
        rb_hash_aset( environment, rb_str_new2( "SERVER_PORT" ), INT2FIX( request.port() ) );

        rb_hash_aset( environment, rb_str_new2( "rack.url_scheme" ), rb_str_new2( "http" ) );
        rb_hash_aset( environment, rb_str_new2( "rack.multithread" ), Qfalse );
        rb_hash_aset( environment, rb_str_new2( "rack.multiprocess" ), Qfalse );
        rb_hash_aset( environment, rb_str_new2( "rack.run_once" ), Qfalse );

        fill_http_headers( environment, request );
    }

    extern "C"
    {
        static VALUE call_ruby_cb( VALUE* params )
        {
            static const ID func_name = rb_intern("call");

            assert( TYPE( params[ 1 ] ) == T_HASH );
            return rb_funcall( params[ 0 ], func_name, 1, params[ 1 ] );
        }

        static VALUE rescue_ruby( VALUE /* arg */, VALUE exception )
        {
            VALUE error_msg = rb_str_new2( "error calling application: " );
            error_msg = rb_str_concat( error_msg, rb_funcall( exception, rb_intern( "message" ), 0 ) );
            error_msg = rb_str_concat( error_msg, rb_str_new2( "\n" ) );

            VALUE back_trace = rb_funcall( exception, rb_intern( "backtrace" ), 0 );
            back_trace = rb_funcall( back_trace, rb_intern( "join" ), 1, rb_str_new2( "\n" ) );

            error_msg = rb_str_concat( error_msg, back_trace );

            return error_msg;
        }
    }

    std::vector< char > bayeux_server::call( const std::vector< char >& body, const http::request_header& request )
    {
        VALUE hash = rb_hash_new();

        fill_header( hash, request );
        rb_hash_aset( hash, rb_str_new2( "rack.input" ), rb_str_new( &body[ 0 ], body.size() ) );

        VALUE func_args[ 2 ] = { app_, hash };

        // call the application callback
        VALUE ruby_result = rb_rescue2(
            reinterpret_cast< VALUE (*)( ANYARGS ) >( &call_ruby_cb ), reinterpret_cast< VALUE >( &func_args[ 0 ] ),
            reinterpret_cast< VALUE (*)( ANYARGS ) >( &rescue_ruby ), Qnil,
            rb_eException, static_cast< VALUE >( 0 ) );

        if ( TYPE( ruby_result ) == T_STRING )
        {
            std::cerr << rack::rb_str_to_sub( ruby_result ) << std::endl;
            return std::vector< char >();
        }

        assert ( TYPE( ruby_result ) == T_ARRAY );
        const int result_size = RARRAY_LEN( ruby_result );

        if ( result_size  == 0 )
        {
            ruby_land_queue_.stop();
            return std::vector< char >();
        }

        assert( result_size == 4 );
        VALUE ruby_error   = rb_ary_pop( ruby_result );
        VALUE ruby_body    = rb_ary_pop( ruby_result );
        VALUE ruby_headers = rb_ary_pop( ruby_result );
        VALUE ruby_status  = rb_ary_pop( ruby_result );

        assert( TYPE( ruby_error ) == T_STRING );
        assert( TYPE( ruby_body ) == T_STRING );
        assert( TYPE( ruby_headers ) == T_STRING );
        assert( TYPE( ruby_status ) == T_FIXNUM );

        const std::string status_line = http::status_line( "1.1", static_cast< http::http_error_code >( NUM2INT( ruby_status ) ) );

        VALUE result = rb_str_new( status_line.data(), status_line.size() );
        result = rb_str_plus( result, ruby_headers );
        result = rb_str_plus( result, ruby_body );

        if ( RSTRING_LEN( ruby_error ) )
            std::cerr << rack::rb_str_to_sub( ruby_error ) << std::endl;

        return std::vector< char >( RSTRING_PTR( result ), RSTRING_PTR( result ) + RSTRING_LEN( result ) );
    }
}

extern "C" VALUE update_node_bayeux( VALUE self, VALUE node, VALUE value )
{
    const pubsub::node_name node_name  = hash_to_node( node );
    const json::value       node_value = ruby_to_json( value, node_name );

    LOG_DETAIL( log_context << "update: " << node_name << " to " << node_value );

    bayeux_server* server_ptr = 0;
    Data_Get_Struct( self, bayeux_server, server_ptr );
    assert( server_ptr );

    server_ptr->update_node( node_name, node_value );

    return self;
}

extern "C" VALUE run_bayeux( VALUE self, VALUE application, VALUE configuration )
{
    VALUE result = Qfalse;

    try
    {
        bayeux_server server( application, self, configuration );

        rack::local_data_ptr local_ptr( self, server );
        server.run();

        result = Qtrue;
    }
    catch ( const std::exception& e )
    {
        rb_raise( rb_eRuntimeError, "exception calling Rack::Handler::Sioux.run(): %s", e.what() );
    }
    catch ( ... )
    {
        rb_raise( rb_eRuntimeError, "unknown exception calling Rack::Handler::Sioux.run()" );
    }

    return result;
}

extern "C" void mark_bayeux( void* server )
{
    static_cast< bayeux_server* >( server )->mark_ruby_references();
}

extern "C" VALUE alloc_bayeux( VALUE klass )
{
    return Data_Wrap_Struct( klass, mark_bayeux, 0, 0 );
}

extern "C" VALUE subscribe_bayeux( VALUE self, VALUE ruby_node )
{
    bayeux_server* server_ptr = 0;
    Data_Get_Struct( self, bayeux_server, server_ptr );

    if ( server_ptr )
        server_ptr->subscribe_test( hash_to_node( ruby_node ) );

    return self;
}

extern "C" void Init_bayeux()
{
    const VALUE mod_sioux = rb_define_module_under( rb_define_module( "Rack" ), "Sioux" );
    VALUE class_    = rb_define_class_under( mod_sioux, "SiouxRubyImplementation", rb_cObject );

    rb_define_alloc_func( class_, alloc_bayeux );
    rb_define_method( class_, "run", RUBY_METHOD_FUNC( run_bayeux ), 2 );
    rb_define_method( class_, "[]=", RUBY_METHOD_FUNC( update_node_bayeux ), 2 );
    rb_define_method( class_, "subscribe_for_testing", RUBY_METHOD_FUNC( subscribe_bayeux ), 1 );
}

