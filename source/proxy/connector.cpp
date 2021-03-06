// Copyright (c) Torrox GmbH & Co KG. All rights reserved.
// Please note that the content of this file is confidential or protected by law.
// Any unauthorised copying or unauthorised distribution of the information contained herein is prohibited.

#include "proxy/connector.h"

namespace proxy {

//////////////////////////////
// class proxy_connection_limit_reached
connection_limit_reached::connection_limit_reached(const std::string& s)
 : std::runtime_error(s)
{
}

//////////////////////////////
// class configuration

configuration::configuration()
 : max_connections_(20u)
 , max_idle_time_(boost::posix_time::seconds(5 * 60))
 , connect_timeout_(boost::posix_time::seconds(5))
 , orgin_timeout_(boost::posix_time::seconds(1))
{
}

unsigned configuration::max_connections() const
{
    return max_connections_;
}

void configuration::max_connections(unsigned val)
{
    max_connections_ = val;
}

boost::posix_time::time_duration configuration::max_idle_time() const
{
    return max_idle_time_;
}

void configuration::max_idle_time(const boost::posix_time::time_duration& val)
{
    max_idle_time_ = val;
}

boost::posix_time::time_duration configuration::connect_timeout() const
{
    return connect_timeout_;
}

void configuration::connect_timeout(const boost::posix_time::time_duration& val)
{
    connect_timeout_ = val;
}

boost::posix_time::time_duration configuration::orgin_timeout() const
{
    return orgin_timeout_;
}

void configuration::orgin_timeout(const boost::posix_time::time_duration& val)
{
    orgin_timeout_ = val;
}

//////////////////////////////
// class configurator
configurator::configurator()
 : config_(new configuration())
{
}

const configurator& configurator::max_connections(unsigned val) const
{
    config_->max_connections(val);

    return *this;
}

const configurator& configurator::max_idle_time(const boost::posix_time::time_duration& val) const
{
    config_->max_idle_time(val);

    return *this;
}

const configurator& configurator::connect_timeout(const boost::posix_time::time_duration& val) const
{
    config_->connect_timeout(val);

    return *this;
}

const configurator& configurator::orgin_timeout(const boost::posix_time::time_duration& val) const
{
    config_->orgin_timeout(val);

    return *this;
}

configurator::operator const configuration&() const
{
    return *config_;
}



} // namespace server

