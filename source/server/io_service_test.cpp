// Copyright (c) Torrox GmbH & Co KG. All rights reserved.
// Please note that the content of this file is confidential or protected by law.
// Any unauthorised copying or unauthorised distribution of the information contained herein is prohibited.

#include "unittest++/unittest++.h"
#include "server/test_tools.h"
#include <boost/asio.hpp>
#include <boost/bind.hpp>

namespace {
	void action2(boost::asio::io_service& q, int& c)
	{
		++c;
	}

	void action1(boost::asio::io_service& q, int& c)
	{
		++c;
		q.post(boost::bind(&action2, boost::ref(q), boost::ref(c)));
	}
}

TEST(queue_multiple_actions)
{
	boost::asio::io_service	queue;
	int counter = 0;
	server::test::run(queue);

	queue.post(boost::bind(&action1, boost::ref(queue), boost::ref(counter)));

	server::test::run(queue);
 	CHECK_EQUAL(2, counter);
}
