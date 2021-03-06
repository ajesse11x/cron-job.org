/*
 * chronos, the cron-job.org execution daemon
 * Copyright (C) 2017-2019 Patrick Schlangen <patrick@schlangen.me>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 */

#include "NodeService.h"

#include <iostream>

#include <thrift/protocol/TBinaryProtocol.h>
#include <thrift/server/TThreadedServer.h>
#include <thrift/transport/TServerSocket.h>
#include <thrift/transport/TBufferTransports.h>

#include "ChronosNode.h"
#include "App.h"
#include "Utils.h"
#include "SQLite.h"

using namespace ::apache::thrift;
using namespace ::apache::thrift::protocol;
using namespace ::apache::thrift::transport;
using namespace ::apache::thrift::server;

namespace {

class ChronosNodeHandler : virtual public ChronosNodeIf
{
public:
    ChronosNodeHandler()
        : userDbFilePathScheme(Chronos::App::getInstance()->config->get("user_db_file_path_scheme")),
          userDbFileNameScheme(Chronos::App::getInstance()->config->get("user_db_file_name_scheme"))
    {
    }

    bool ping() override
    {
        std::cout << "ChronosNodeHandler::ping()" << std::endl;
        return true;
    }

    void getJobsForUser(std::vector<Job> &_return, const int64_t userId) override
    {
        using namespace Chronos;

        std::cout << "ChronosNodeHandler::getJobsForUser(" << userId << ")" << std::endl;

        try
        {
            std::unique_ptr<MySQL_DB> db(App::getInstance()->createMySQLConnection());

	        MYSQL_ROW row;
            auto res = db->query("SELECT `jobid`,`userid`,`enabled`,`title`,`save_responses`,`last_status`,`last_fetch`,`last_duration`,`fail_counter`,`url`,`request_method`,`timezone` FROM `job` WHERE `userid`=%v",
                userId);
            _return.reserve(res->numRows());
            while((row = res->fetchRow()))
            {
                Job job;

                job.identifier.jobId = std::stoll(row[0]);
                job.identifier.userId = std::stoll(row[1]);

                job.metaData.enabled = std::strcmp(row[2], "1") == 0;
                job.metaData.title = row[3];
                job.metaData.saveResponses = std::strcmp(row[4], "1") == 0;
                job.__isset.metaData = true;

                job.executionInfo.lastStatus = static_cast<JobStatus::type>(std::stoi(row[5])); //!< @todo Nicer conversion
                job.executionInfo.lastFetch = std::stoll(row[6]);
                job.executionInfo.lastDuration = std::stoi(row[7]);
                job.executionInfo.failCounter = std::stoi(row[8]);
                job.__isset.executionInfo = true;

                job.data.url = row[9];
                job.data.requestMethod = static_cast<RequestMethod::type>(std::stoi(row[10])); //!< @todo Nicer conversion
                job.__isset.data = true;

                job.schedule.timezone = row[11];
                job.__isset.schedule = true;

                getJobSchedule(db, job.identifier, "hour",      job.schedule.hours);
                getJobSchedule(db, job.identifier, "mday",      job.schedule.mdays);
                getJobSchedule(db, job.identifier, "minute",    job.schedule.minutes);
                getJobSchedule(db, job.identifier, "month",     job.schedule.months);
                getJobSchedule(db, job.identifier, "wday",      job.schedule.wdays);

                _return.push_back(job);
            }
        }
        catch(const std::exception &ex)
        {
            std::cout << "ChronosNodeHandler::getJobsForUser(): Exception: "  << ex.what() << std::endl;
            throw InternalError();
        }
    }

    void getJobDetails(Job &_return, const JobIdentifier &identifier) override
    {
        using namespace Chronos;

        std::cout << "ChronosNodeHandler::getJobDetails(" << identifier.jobId << ", " << identifier.userId << ")" << std::endl;

        try
        {
            std::unique_ptr<MySQL_DB> db(App::getInstance()->createMySQLConnection());

	        MYSQL_ROW row;
            auto res = db->query("SELECT `jobid`,`userid`,`enabled`,`title`,`save_responses`,`last_status`,`last_fetch`,"
                    "`last_duration`,`fail_counter`,`url`,`request_method`,`auth_enable`,`auth_user`,`auth_pass`,"
                    "`notify_failure`,`notify_success`,`notify_disable`,`timezone` "
                    "FROM `job` WHERE `jobid`=%v AND `userid`=%v",
                identifier.jobId,
                identifier.userId);
            if(res->numRows() == 0)
                throw ResourceNotFound();
            while((row = res->fetchRow()))
            {
                _return.identifier.jobId = std::stoll(row[0]);
                _return.identifier.userId = std::stoll(row[1]);

                _return.metaData.enabled = std::strcmp(row[2], "1") == 0;
                _return.metaData.title = row[3];
                _return.metaData.saveResponses = std::strcmp(row[4], "1") == 0;
                _return.__isset.metaData = true;

                _return.executionInfo.lastStatus = static_cast<JobStatus::type>(std::stoi(row[5])); //!< @todo Nicer conversion
                _return.executionInfo.lastFetch = std::stoll(row[6]);
                _return.executionInfo.lastDuration = std::stoi(row[7]);
                _return.executionInfo.failCounter = std::stoi(row[8]);
                _return.__isset.executionInfo = true;

                _return.data.url = row[9];
                _return.data.requestMethod = static_cast<RequestMethod::type>(std::stoi(row[10])); //!< @todo Nicer conversion
                _return.__isset.data = true;

                _return.authentication.enable = std::strcmp(row[11], "1") == 0;
                _return.authentication.user = row[12];
                _return.authentication.password = row[13];
                _return.__isset.authentication = true;

                _return.notification.onFailure = std::strcmp(row[14], "1") == 0;
                _return.notification.onSuccess = std::strcmp(row[15], "1") == 0;
                _return.notification.onDisable = std::strcmp(row[16], "1") == 0;
                _return.__isset.notification = true;

                _return.schedule.timezone = row[17];
                _return.__isset.schedule = true;
            }

            getJobSchedule(db, identifier, "hour",      _return.schedule.hours);
            getJobSchedule(db, identifier, "mday",      _return.schedule.mdays);
            getJobSchedule(db, identifier, "minute",    _return.schedule.minutes);
            getJobSchedule(db, identifier, "month",     _return.schedule.months);
            getJobSchedule(db, identifier, "wday",      _return.schedule.wdays);

            res = db->query("SELECT `body` FROM `job_body` WHERE `jobid`=%v",
                identifier.jobId);
            while((row = res->fetchRow()))
            {
                _return.extendedData.body = row[0];
            }

            res = db->query("SELECT `key`,`value` FROM `job_header` WHERE `jobid`=%v",
                identifier.jobId);
            while((row = res->fetchRow()))
            {
                _return.extendedData.headers.emplace(row[0], row[1]);
            }

            _return.__isset.extendedData = true;
        }
        catch(const std::exception &ex)
        {
            std::cout << "ChronosNodeHandler::getJobDetails(): Exception: "  << ex.what() << std::endl;
            throw InternalError();
        }
    }

    void createOrUpdateJob(const Job &job) override
    {
        using namespace Chronos;

        std::cout << "ChronosNodeHandler::createOrUpdateJob(" << job.identifier.jobId << ", " << job.identifier.userId << ")" << std::endl;

        if(job.identifier.userId <= 0 || job.identifier.jobId <= 0)
            throw InvalidArguments();

        try
        {
            std::unique_ptr<MySQL_DB> db(App::getInstance()->createMySQLConnection());

            db->query("BEGIN");

            const auto jobUser = jobUserId(db, job.identifier.jobId);
            if(jobUser == -1)
            {
                db->query("INSERT INTO `job`(`jobid`,`userid`) VALUES(%v,%v)",
                    job.identifier.jobId,
                    job.identifier.userId);
            }
            else if (jobUser != job.identifier.userId)
            {
                throw Forbidden();
            }

            if(job.__isset.metaData)
            {
                db->query("UPDATE `job` SET `enabled`=%d, `title`='%q', `save_responses`=%d WHERE `jobid`=%v",
                    job.metaData.enabled ? 1 : 0,
                    job.metaData.title.c_str(),
                    job.metaData.saveResponses ? 1 : 0,
                    job.identifier.jobId);
            }

            if(job.__isset.authentication)
            {
                db->query("UPDATE `job` SET `auth_enable`=%d, `auth_user`='%q', `auth_pass`='%q' WHERE `jobid`=%v",
                    job.authentication.enable ? 1 : 0,
                    job.authentication.user.c_str(),
                    job.authentication.password.c_str(),
                    job.identifier.jobId);
            }

            if(job.__isset.notification)
            {
                db->query("UPDATE `job` SET `notify_failure`=%d, `notify_success`=%d, `notify_disable`=%d WHERE `jobid`=%v",
                    job.notification.onFailure ? 1 : 0,
                    job.notification.onSuccess ? 1 : 0,
                    job.notification.onDisable ? 1 : 0,
                    job.identifier.jobId);
            }

            if(job.__isset.schedule)
            {
                db->query("UPDATE `job` SET `timezone`='%q' WHERE `jobid`=%v",
                    job.schedule.timezone.c_str(),
                    job.identifier.jobId);

                saveJobSchedule(db, job.identifier, "hour",      job.schedule.hours);
                saveJobSchedule(db, job.identifier, "mday",      job.schedule.mdays);
                saveJobSchedule(db, job.identifier, "minute",    job.schedule.minutes);
                saveJobSchedule(db, job.identifier, "month",     job.schedule.months);
                saveJobSchedule(db, job.identifier, "wday",      job.schedule.wdays);
            }

            if(job.__isset.data)
            {
                db->query("UPDATE `job` SET `url`='%q',`request_method`=%d WHERE `jobid`=%v",
                    job.data.url.c_str(),
                    static_cast<int>(job.data.requestMethod),   //!< @todo Nicer conversion
                    job.identifier.jobId);
            }

            if(job.__isset.extendedData)
            {
                if(Utils::trim(job.extendedData.body).length() == 0)
                {
                    db->query("DELETE FROM `job_body` WHERE `jobid`=%v",
                        job.identifier.jobId);
                }
                else
                {
                    db->query("REPLACE INTO `job_body`(`jobid`,`body`) VALUES(%v,'%q')",
                        job.identifier.jobId,
                        job.extendedData.body.c_str());
                }

                db->query("DELETE FROM `job_header` WHERE `jobid`=%v",
                    job.identifier.jobId);
                for(const auto &header : job.extendedData.headers)
                {
                    db->query("INSERT INTO `job_header`(`jobid`,`key`,`value`) VALUES(%v,'%q','%q')",
                        job.identifier.jobId,
                        header.first.c_str(),
                        header.second.c_str());
                }
            }

            db->query("COMMIT");
        }
        catch(const std::exception &ex)
        {
            std::cout << "ChronosNodeHandler::createOrUpdateJob(): Exception: "  << ex.what() << std::endl;
            throw InternalError();
        }
    }

    void deleteJob(const JobIdentifier &identifier) override
    {
        using namespace Chronos;

        std::cout << "ChronosNodeHandler::deleteJob(" << identifier.jobId << ", " << identifier.userId << ")" << std::endl;

        try
        {
            std::unique_ptr<MySQL_DB> db(App::getInstance()->createMySQLConnection());

            if(!jobExists(db, identifier))
                throw ResourceNotFound();

            db->query("BEGIN");
            db->query("DELETE FROM `notification` WHERE `jobid`=%v",    identifier.jobId);
            db->query("DELETE FROM `job_hours` WHERE `jobid`=%v",       identifier.jobId);
            db->query("DELETE FROM `job_mdays` WHERE `jobid`=%v",       identifier.jobId);
            db->query("DELETE FROM `job_minutes` WHERE `jobid`=%v",     identifier.jobId);
            db->query("DELETE FROM `job_months` WHERE `jobid`=%v",      identifier.jobId);
            db->query("DELETE FROM `job_wdays` WHERE `jobid`=%v",       identifier.jobId);
            db->query("DELETE FROM `job_body` WHERE `jobid`=%v",        identifier.jobId);
            db->query("DELETE FROM `job_header` WHERE `jobid`=%v",      identifier.jobId);
            db->query("DELETE FROM `job` WHERE `jobid`=%v",             identifier.jobId);
            db->query("COMMIT");
        }
        catch(const std::exception &ex)
        {
            std::cout << "ChronosNodeHandler::deleteJob(): Exception: "  << ex.what() << std::endl;
            throw InternalError();
        }
    }

    void getJobLog(std::vector<JobLogEntry> &_return, const JobIdentifier &identifier, const int16_t maxEntries) override
    {
        static constexpr const int TIME_ONE_DAY = 86400;

        std::cout << "ChronosNodeHandler::getJobLog(" << identifier.jobId << ", " << identifier.userId << ", " << maxEntries << ")" << std::endl;

        if(maxEntries <= 0)
            throw InvalidArguments();

        try
        {
            //! @note No verification of identifier here since we look in the user DB and thus cannot accidentally fetch data
            //!       for a different user. Also, we need to accept jobId == 0 to fetch logs for all the user's jobs.
            //! @note To account for different time zones, we fetch logs from tomorrow, today and yesterday (from GMT PoV).

            struct tm tmYesterday   = timeStruct(- TIME_ONE_DAY);
            struct tm tmToday       = timeStruct(0);
            struct tm tmTomorrow    = timeStruct(TIME_ONE_DAY);

            getJobLogForDay(_return, identifier, tmTomorrow.tm_mday,    tmTomorrow.tm_mon,  maxEntries);
            getJobLogForDay(_return, identifier, tmToday.tm_mday,       tmToday.tm_mon,     maxEntries - std::min(static_cast<std::size_t>(maxEntries), _return.size()));
            getJobLogForDay(_return, identifier, tmYesterday.tm_mday,   tmYesterday.tm_mon, maxEntries - std::min(static_cast<std::size_t>(maxEntries), _return.size()));
        }
        catch(const std::exception &ex)
        {
            std::cout << "ChronosNodeHandler::getJobLog(): Exception: "  << ex.what() << std::endl;
            throw InternalError();
        }
    }

    void getJobLogDetails(JobLogEntry &_return, const int64_t userId, const int16_t mday, const int16_t month, const int64_t jobLogId) override
    {
        using namespace Chronos;

        std::cout << "ChronosNodeHandler::getJobLogDetails(" << userId << ", " << mday << ", " << month << ", " << jobLogId << ")" << std::endl;

        std::string dbFilePath = Utils::userDbFilePath(userDbFilePathScheme, userDbFileNameScheme, userId, mday, month);
        std::unique_ptr<SQLite_DB> userDB;

        try
        {
            userDB = std::make_unique<SQLite_DB>(dbFilePath.c_str(), true /* read only */);

            auto stmt = userDB->prepare("SELECT * FROM \"joblog\" LEFT JOIN \"joblog_response\" ON \"joblog_response\".\"joblogid\"=\"joblog\".\"joblogid\" WHERE \"joblog\".\"joblogid\"=:joblogid");
            stmt->bind(":joblogid", jobLogId);

            while(stmt->execute())
            {
                _return = convertToJobLogEntry(stmt, userId, mday, month);

                if(stmt->hasField("headers"))
                    _return.headers = stmt->stringValue("headers");
                if(stmt->hasField("body"))
                    _return.body = stmt->stringValue("body");

                _return.__isset.headers = true;
                _return.__isset.body = true;

                return;
            }

            throw ResourceNotFound();
        }
        catch(const std::exception &ex)
        {
            throw ResourceNotFound();
        }
    }

    void getNotifications(std::vector<NotificationEntry> &_return, const int64_t userId, const int16_t maxEntries) override
    {
        using namespace Chronos;

        std::cout << "ChronosNodeHandler::getNotifications(" << userId << ")" << std::endl;

        if(userId <= 0)
            throw InvalidArguments();

        try
        {
            std::unique_ptr<MySQL_DB> db(App::getInstance()->createMySQLConnection());

	        MYSQL_ROW row;
            auto res = db->query("SELECT `notification`.`joblogid`,`notification`.`jobid`,`job`.`userid`,`notification`.`date`,`notification`.`type`,`notification`.`date_started`,`notification`.`date_planned`,`notification`.`url`,`notification`.`execution_status`,`notification`.`execution_status_text`,`notification`.`execution_http_status` "
                "FROM `notification` "
                "INNER JOIN `job` ON `job`.`jobid`=`notification`.`jobid` "
                "WHERE `job`.`userid`=%v "
                "ORDER BY `notification`.`notificationid` DESC LIMIT %u",
                userId,
                static_cast<unsigned long>(maxEntries));
            _return.reserve(res->numRows());
            while((row = res->fetchRow()))
            {
                NotificationEntry n;

                n.notificationId = std::stoll(row[0]);

                n.jobIdentifier.jobId = std::stoll(row[1]);
                n.jobIdentifier.userId = std::stoll(row[2]);

                n.date = std::stoll(row[3]);

                n.type = static_cast<NotificationType::type>(std::stoi(row[4])); //!< @todo Nicer conversion

                n.dateStarted = std::stoll(row[5]);
                n.datePlanned = std::stoll(row[6]);

                n.url = row[7];

                n.executionStatus = static_cast<JobStatus::type>(std::stoi(row[8])); //!< @todo Nicer conversion
                n.executionStatusText = row[9];
                n.httpStatus = std::stoi(row[10]);

                _return.push_back(n);
            }
        }
        catch(const std::exception &ex)
        {
            std::cout << "ChronosNodeHandler::getNotifications(): Exception: "  << ex.what() << std::endl;
            throw InternalError();
        }
    }

    void disableJobsForUser(const int64_t userId) override
    {
        using namespace Chronos;

        std::cout << "ChronosNodeHandler::disableJobsForUser(" << userId << ")" << std::endl;

        try
        {
            std::unique_ptr<MySQL_DB> db(App::getInstance()->createMySQLConnection());

            db->query("UPDATE `job` SET `enabled`=0 WHERE `userid`=%v",
                userId);
        }
        catch(const std::exception &ex)
        {
            std::cout << "ChronosNodeHandler::disableJobsForUser(): Exception: "  << ex.what() << std::endl;
            throw InternalError();
        }
    }

private:
    template<typename T>
    void getJobSchedule(std::unique_ptr<Chronos::MySQL_DB> &db, const JobIdentifier &identifier, const char *name, std::set<T> &target) const
    {
        MYSQL_ROW row;
        auto res = db->query("SELECT `%s` FROM `job_%ss` WHERE `jobid`=%v",
            name, name,
            identifier.jobId);
        while((row = res->fetchRow()))
        {
            target.insert(std::stoi(row[0]));
        }
    }

    template<typename T>
    void saveJobSchedule(std::unique_ptr<Chronos::MySQL_DB> &db, const JobIdentifier &identifier, const char *name, const std::set<T> &items) const
    {
        db->query("DELETE FROM `job_%ss` WHERE `jobid`=%v",
            name,
            identifier.jobId);

        for(const auto &val : items)
        {
            db->query("INSERT INTO `job_%ss`(`jobid`,`%s`) VALUES(%v,%d)",
                name, name,
                identifier.jobId,
                val);
        }
    }

    long long jobUserId(std::unique_ptr<Chronos::MySQL_DB> &db, const long long jobId) const
    {
        MYSQL_ROW row;
        auto res = db->query("SELECT `userid` FROM `job` WHERE `jobid`=%v",
            jobId);
        while((row = res->fetchRow()))
        {
            return std::stoll(row[0]);
        }
        return(-1);
    }

    bool jobExists(std::unique_ptr<Chronos::MySQL_DB> &db, const JobIdentifier &identifier) const
    {
        const auto userId = jobUserId(db, identifier.jobId);
        return(userId != -1 && userId == identifier.userId);
    }

    struct tm timeStruct(const int offsetFromNow) const
    {
        struct tm tmStruct = { 0 };
        time_t tmTime = time(nullptr) + offsetFromNow;
        if(gmtime_r(&tmTime, &tmStruct) == nullptr)
            throw std::runtime_error("gmtime_r returned nullptr");
        return tmStruct;
    }

    void getJobLogForDay(std::vector<JobLogEntry> &_return, const JobIdentifier &identifier, const int mday, const int month, const int16_t maxEntries) const
    {
        using namespace Chronos;

        if(maxEntries == 0)
            return;

        std::string dbFilePath = Utils::userDbFilePath(userDbFilePathScheme, userDbFileNameScheme, identifier.userId, mday, month);
        std::unique_ptr<SQLite_DB> userDB;

        try
        {
            userDB = std::make_unique<SQLite_DB>(dbFilePath.c_str(), true /* read only */);
        }
        catch(const std::exception &ex)
        {
            //! @note Ignore failures during open (the db probably doesn't exist because there's no log entry on that day)
            return;
        }

        std::string query = "SELECT \"joblogid\",\"jobid\",\"date\",\"date_planned\",\"jitter\",\"url\",\"duration\",\"status\",\"status_text\",\"http_status\" FROM \"joblog\" ";
        if(identifier.jobId > 0)
        {
            query += "WHERE \"jobid\"=:jobid ";
        }
        query += "ORDER BY \"joblogid\" DESC LIMIT " + std::to_string(maxEntries);

        auto stmt = userDB->prepare(query);
        if(identifier.jobId > 0)
        {
            stmt->bind(":jobid", identifier.jobId);
        }

        while(stmt->execute())
        {
            _return.push_back(convertToJobLogEntry(stmt, identifier.userId, mday, month));
        }
    }

    JobLogEntry convertToJobLogEntry(const std::unique_ptr<Chronos::SQLite_Statement> &stmt, int64_t userId, int16_t mday, int16_t month) const
    {
        JobLogEntry entry;
        entry.jobLogId                  = stmt->intValue("joblogid");
        entry.jobIdentifier.userId      = userId;
        entry.jobIdentifier.jobId       = stmt->intValue("jobid");
        entry.date                      = stmt->intValue("date");
        entry.datePlanned               = stmt->intValue("date_planned");
        entry.jitter                    = stmt->intValue("jitter");
        entry.url                       = stmt->stringValue("url");
        entry.duration                  = stmt->intValue("duration");
        entry.status                    = static_cast<JobStatus::type>(stmt->intValue("status")); //!< @todo Nicer conversion
        entry.statusText                = stmt->stringValue("status_text");
        entry.httpStatus                = stmt->intValue("http_status");
        entry.mday                      = mday;
        entry.month                     = month;
        return entry;
    }

    std::string userDbFilePathScheme;
    std::string userDbFileNameScheme;
};

}

namespace Chronos {

NodeService::NodeService(const std::string &interface, int port)
    : server(std::make_shared<TThreadedServer>(
        std::make_shared<ChronosNodeProcessor>(std::make_shared<ChronosNodeHandler>()),
        std::make_shared<TServerSocket>(interface, port),
        std::make_shared<TBufferedTransportFactory>(),
        std::make_shared<TBinaryProtocolFactory>()
    ))
{
}

void NodeService::run()
{
    server->serve();
}

void NodeService::stop()
{
    server->stop();
}

} // Chronos