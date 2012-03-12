// Copyright (c) Torrox GmbH & Co KG. All rights reserved.
// Please note that the content of this file is confidential or protected by law.
// Any unauthorised copying or unauthorised distribution of the information contained herein is prohibited.

#include "bayeux/bayeux.h"

#include <boost/bind.hpp>

#include "bayeux/configuration.h"
#include "bayeux/session.h"
#include "server/test_timer.h"
#include "tools/scope_guard.h"

namespace bayeux
{
    template < class Timer >
    connector< Timer >::connector( boost::asio::io_service& queue, pubsub::root& data,
	        server::session_generator& session_generator, const configuration& config )
		: queue_( queue )
	    , data_( data )
        , mutex_()
		, session_generator_( session_generator )
		, current_config_( new configuration( config ) )
		, sessions_()
		, index_()
	{
	}

    template < class Timer >
	session* connector< Timer >::find_session( const json::string& session_id )
	{
        boost::mutex::scoped_lock lock( mutex_ );

		const typename session_list_t::iterator pos = sessions_.find( session_id.to_std_string() );

		if ( pos != sessions_.end() )
		{
		    ++pos->second.use_count_;
		    return pos->second.session_.get();
		}

		return 0;
	}


    template < class Timer >
	session* connector< Timer >::create_session( const std::string& network_connection_name )
	{
        boost::mutex::scoped_lock lock( mutex_ );
        std::string session_id = session_generator_( network_connection_name );

        for ( ; sessions_.find( session_id ) != sessions_.end(); session_id = session_generator_( network_connection_name ) )
            ;

        const session_data data( session_id, data_, current_config_, queue_ );
        const typename session_list_t::iterator pos = sessions_.insert( std::make_pair( session_id, data ) ).first;

        tools::scope_guard remove_session_if_index_fails =
            tools::make_obj_guard( *this, &connector< Timer >::remove_from_sessions, pos );

        session* const result = data.session_.get();
        index_.insert( std::make_pair( result, pos ) );

        remove_session_if_index_fails.dismiss();

		return result;
	}

    template < class Timer >
    void connector< Timer >::idle_session( const session* session )
    {
        boost::mutex::scoped_lock lock( mutex_ );

        const typename session_index_t::iterator pos = index_.find( session );
        assert( pos != index_.end() );
        assert( pos->second->second.use_count_ > 0 );

        if ( --pos->second->second.use_count_ == 0 )
        {
            Timer& timer = *pos->second->second.timer_;
            timer.expires_from_now( current_config_->session_timeout() );
            timer.async_wait(
                boost::bind( &connector< Timer >::session_timeout_reached, boost::ref( *this ), session, _1 ) );
        }
    }

    template < class Timer >
    void connector< Timer >::drop_session( const json::string& session_id )
    {
        boost::mutex::scoped_lock lock( mutex_ );

        const typename session_list_t::iterator pos = sessions_.find( session_id.to_std_string() );

        if ( pos != sessions_.end() && pos->second.use_count_ == 0 )
        {
            const typename session_index_t::size_type delete_index_elements =
                index_.erase( pos->second.session_.get() );

            static_cast< void >( delete_index_elements );
            assert( delete_index_elements == 1 );

            sessions_.erase( pos );
        }

        assert( sessions_.size() == index_.size() );
    }

    template < class Timer >
    void connector< Timer >::remove_from_sessions( typename session_list_t::iterator pos )
    {
        sessions_.erase( pos );
    }

    template < class Timer >
    void connector< Timer >::session_timeout_reached( const session* s, const boost::system::error_code& ec )
    {
        if ( ec )
            return;

        boost::mutex::scoped_lock lock( mutex_ );
        const typename session_index_t::iterator pos = index_.find( s );

        if ( pos != index_.end() && pos->second->second.use_count_ == 0 )
        {
            sessions_.erase( pos->second );
            index_.erase( pos );

            assert( sessions_.size() == index_.size() );
        }
    }

    template < class Timer >
    connector< Timer >::session_data::session_data( const std::string& session_id, pubsub::root& data,
        const boost::shared_ptr< const configuration >& config, boost::asio::io_service& queue )
        : use_count_( 1u )
        , remove_( false )
        , session_( new session( session_id, data, config ) )
        , timer_( new Timer ( queue ) )
    {
    }

    template class connector<>;
    template class connector< server::test::timer >;
}

