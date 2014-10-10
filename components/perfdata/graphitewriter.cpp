/******************************************************************************
 * Icinga 2                                                                   *
 * Copyright (C) 2012-2014 Icinga Development Team (http://www.icinga.org)    *
 *                                                                            *
 * This program is free software; you can redistribute it and/or              *
 * modify it under the terms of the GNU General Public License                *
 * as published by the Free Software Foundation; either version 2             *
 * of the License, or (at your option) any later version.                     *
 *                                                                            *
 * This program is distributed in the hope that it will be useful,            *
 * but WITHOUT ANY WARRANTY; without even the implied warranty of             *
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the              *
 * GNU General Public License for more details.                               *
 *                                                                            *
 * You should have received a copy of the GNU General Public License          *
 * along with this program; if not, write to the Free Software Foundation     *
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA.             *
 ******************************************************************************/

#include "perfdata/graphitewriter.hpp"
#include "icinga/service.hpp"
#include "icinga/macroprocessor.hpp"
#include "icinga/icingaapplication.hpp"
#include "icinga/compatutility.hpp"
#include "icinga/perfdatavalue.hpp"
#include "base/tcpsocket.hpp"
#include "base/dynamictype.hpp"
#include "base/objectlock.hpp"
#include "base/logger_fwd.hpp"
#include "base/convert.hpp"
#include "base/utility.hpp"
#include "base/application.hpp"
#include "base/stream.hpp"
#include "base/networkstream.hpp"
#include "base/exception.hpp"
#include "base/statsfunction.hpp"
#include <boost/algorithm/string.hpp>
#include <boost/algorithm/string/classification.hpp>
#include <boost/foreach.hpp>
#include <boost/algorithm/string/split.hpp>
#include <boost/algorithm/string/replace.hpp>

using namespace icinga;

REGISTER_TYPE(GraphiteWriter);

REGISTER_STATSFUNCTION(GraphiteWriterStats, &GraphiteWriter::StatsFunc);

Value GraphiteWriter::StatsFunc(const Dictionary::Ptr& status, const Array::Ptr&)
{
	Dictionary::Ptr nodes = make_shared<Dictionary>();

	BOOST_FOREACH(const GraphiteWriter::Ptr& graphitewriter, DynamicType::GetObjectsByType<GraphiteWriter>()) {
		nodes->Set(graphitewriter->GetName(), 1); //add more stats
	}

	status->Set("graphitewriter", nodes);

	return 0;
}

void GraphiteWriter::Start(void)
{
	DynamicObject::Start();

	m_ReconnectTimer = make_shared<Timer>();
	m_ReconnectTimer->SetInterval(10);
	m_ReconnectTimer->OnTimerExpired.connect(boost::bind(&GraphiteWriter::ReconnectTimerHandler, this));
	m_ReconnectTimer->Start();
	m_ReconnectTimer->Reschedule(0);

	Service::OnNewCheckResult.connect(boost::bind(&GraphiteWriter::CheckResultHandler, this, _1, _2));
}

void GraphiteWriter::ReconnectTimerHandler(void)
{
	if (m_Stream)
		return;

	TcpSocket::Ptr socket = make_shared<TcpSocket>();

	Log(LogNotice, "GraphiteWriter", "Reconnect to tcp socket on host '" + GetHost() + "' port '" + GetPort() + "'.");

	try {
		socket->Connect(GetHost(), GetPort());
	} catch (std::exception&) {
		Log(LogCritical, "GraphiteWriter", "Can't connect to tcp socket on host '" + GetHost() + "' port '" + GetPort() + "'.");
		return;
	}

	m_Stream = make_shared<NetworkStream>(socket);
}

void GraphiteWriter::CheckResultHandler(const Checkable::Ptr& checkable, const CheckResult::Ptr& cr)
{
	CONTEXT("Processing check result for '" + checkable->GetName() + "'");

	if (!IcingaApplication::GetInstance()->GetEnablePerfdata() || !checkable->GetEnablePerfdata())
		return;

	Service::Ptr service = dynamic_pointer_cast<Service>(checkable);
	Host::Ptr host;

	if (service)
		host = service->GetHost();
	else
		host = static_pointer_cast<Host>(checkable);

	String hostName = host->GetName();
	SanitizeMetric(hostName);

	String prefix;

	if (service) {
		String serviceName = service->GetShortName();
		SanitizeMetric(serviceName);
		prefix = "icinga." + hostName + "." + serviceName;

		SendMetric(prefix, "state", service->GetState());
	} else {
		prefix = "icinga." + hostName;

		SendMetric(prefix, "state", host->GetState());
	}

	SendMetric(prefix, "current_attempt", checkable->GetCheckAttempt());
	SendMetric(prefix, "max_check_attempts", checkable->GetMaxCheckAttempts());
	SendMetric(prefix, "state_type", checkable->GetStateType());
	SendMetric(prefix, "reachable", checkable->IsReachable());
	SendMetric(prefix, "downtime_depth", checkable->GetDowntimeDepth());
	SendMetric(prefix, "latency", Service::CalculateLatency(cr));
	SendMetric(prefix, "execution_time", Service::CalculateExecutionTime(cr));
	SendPerfdata(prefix, cr);
}

void GraphiteWriter::SendPerfdata(const String& prefix, const CheckResult::Ptr& cr)
{
	Array::Ptr perfdata = cr->GetPerformanceData();

	if (!perfdata)
		return;

	ObjectLock olock(perfdata);
	BOOST_FOREACH(const Value& val, perfdata) {
		PerfdataValue::Ptr pdv;
		
		if (val.IsObjectType<PerfdataValue>())
			pdv = val;
		else {
			try {
				pdv = PerfdataValue::Parse(val);
			} catch (const std::exception&) {
				Log(LogWarning, "GraphiteWriter", "Ignoring invalid perfdata value: " + val);
				continue;
			}
		}
		
		String escaped_key = pdv->GetLabel();
		SanitizeMetric(escaped_key);
		boost::algorithm::replace_all(escaped_key, "::", ".");

		SendMetric(prefix, escaped_key, pdv->GetValue());
	}
}

void GraphiteWriter::SendMetric(const String& prefix, const String& name, double value)
{
	std::ostringstream msgbuf;
	msgbuf << prefix << "." << name << " " << Convert::ToString(value) << " " << static_cast<long>(Utility::GetTime());

	Log(LogDebug, "GraphiteWriter", "Add to metric list:'" + msgbuf.str() + "'.");

	// do not send \n to debug log
	msgbuf << "\n";
	String metric = msgbuf.str();

	ObjectLock olock(this);

	if (!m_Stream)
		return;

	try {
		m_Stream->Write(metric.CStr(), metric.GetLength());
	} catch (const std::exception& ex) {
		Log(LogCritical, "GraphiteWriter", "Cannot write to TCP socket on host '" + GetHost() + "' port '" + GetPort() + "'.");

		m_Stream.reset();
	}
}

void GraphiteWriter::SanitizeMetric(String& str)
{
	boost::replace_all(str, " ", "_");
	boost::replace_all(str, ".", "_");
	boost::replace_all(str, "-", "_");
	boost::replace_all(str, "\\", "_");
	boost::replace_all(str, "/", "_");
}
