/**
 * Copyright (c) 2019-2022 polistern
 *
 * This file is part of pboted and licensed under BSD3
 *
 * See full license text in LICENSE file at top of project tree
 */

#include <arpa/inet.h>
#include <cstring>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include "BoteContext.h"
#include "EmailWorker.h"
#include "FileSystem.h"
#include "Logging.h"
#include "POP3.h"

namespace bote
{
namespace pop3
{

POP3::POP3 (const std::string &address, int port)
  : started (false),
    processing (false),
    pop3_thread (nullptr),
    server_sockfd (-1),
    client_sockfd (-1),
    sin_size (0),
    server_addr (),
    client_addr (),
    session_state (STATE_QUIT)
    
{
  std::memset (&server_addr, 0, sizeof (server_addr));

  server_addr.sin_family = AF_INET;
  server_addr.sin_port = htons (port);
  server_addr.sin_addr.s_addr = inet_addr (address.c_str ());
  bzero (&(server_addr.sin_zero), 8);

  if ((server_sockfd = socket (AF_INET, SOCK_STREAM, 0)) == -1)
    {
      // ToDo: add error handling
      LogPrint (eLogError, "POP3: Socket create error: ", strerror (errno));
    }

  memset (buf, 0, sizeof (buf));
}

POP3::~POP3 ()
{
  stop ();

  pop3_thread->join ();
  
  delete pop3_thread;
  pop3_thread = nullptr;
}

void
POP3::start ()
{
  if (started)
    return;

  if (bind (server_sockfd, (struct sockaddr *)&server_addr,
            sizeof (struct sockaddr)) == -1)
    {
      // ToDo: add error handling
      LogPrint (eLogError, "POP3: Bind error: ", strerror (errno));
    }

  fcntl (server_sockfd, F_SETFL, fcntl (server_sockfd, F_GETFL, 0) | O_NONBLOCK);

  if (listen (server_sockfd, MAX_CLIENTS) == -1)
    {
      // ToDo: add error handling
      LogPrint (eLogError, "POP3: Listen error: ", strerror (errno));
    }

  started = true;
  pop3_thread = new std::thread ([this] { run (); });
}

void
POP3::stop ()
{
  started = false;
  close (server_sockfd);

  LogPrint (eLogInfo, "POP3: Stopped");
}

void
POP3::run ()
{
  LogPrint (eLogInfo, "POP3: Started");
  sin_size = sizeof (client_addr);

  while (started)
    {
      client_sockfd =
        accept (server_sockfd, (struct sockaddr *)&client_addr, &sin_size);

      if (client_sockfd == -1)
        {
          if (errno != EWOULDBLOCK && errno != EAGAIN)
            {
              // ToDo: add error handling
              LogPrint (eLogError, "POP3: Accept error: ", strerror (errno));
            }

          std::this_thread::sleep_for (std::chrono::milliseconds (POP3_WAIT_TIMEOUT));
        }
      else
        {
          LogPrint (eLogInfo, "POP3: Received connection from ",
                    inet_ntoa (client_addr.sin_addr));

          handle ();
        }
    }
}

void
POP3::handle ()
{
  LogPrint (eLogDebug, "POP3session: New session");

  processing = true;
  process ();
}

void
POP3::finish ()
{
  LogPrint (eLogDebug, "POP3session: Finish session");

  processing = false;
  close (client_sockfd);

  LogPrint (eLogInfo, "POP3session: Socket closed");
}

void
POP3::process ()
{
  reply (reply_ok[OK_HELO]);
  session_state = STATE_USER;

  while (processing)
    {
      memset (buf, 0, sizeof (buf));
      ssize_t len = recv (client_sockfd, buf, sizeof (buf), 0);
      if (len > 0)
        {
          std::string str_buf (buf);
          str_buf = str_buf.substr (0, str_buf.size () - 2);

          LogPrint (eLogDebug, "POP3session: Request stream: ", str_buf);
          respond (buf);
        }
      else if (len == 0)
        continue;
      else
        {
          /// The server exit permanently
          // ToDo: add error handling
          LogPrint (eLogError, "POP3session: Can't recieve data, exit");
          processing = false;
        }
    }

  finish ();
}

void
POP3::respond (char *request)
{
  /// POP3 basic
  if (strncmp (request, "USER", 4) == 0)
    {
      USER (request);
    }
  else if (strncmp (request, "PASS", 4) == 0)
    {
      PASS (request);
    }
  else if (strncmp (request, "STAT", 4) == 0)
    {
      STAT ();
    }
  else if (strncmp (request, "LIST", 4) == 0)
    {
      LIST (request);
    }
  else if (strncmp (request, "RETR", 4) == 0)
    {
      RETR (request);
    }
  else if (strncmp (request, "DELE", 4) == 0)
    {
      DELE (request);
    }
  else if (strncmp (request, "NOOP", 4) == 0)
    {
      NOOP ();
    }
  else if (strncmp (request, "RSET", 4) == 0)
    {
      RSET ();
    }
  else if (strncmp (request, "QUIT", 4) == 0)
    {
      QUIT ();
    }
  /// Extensions RFC 2449
  else if (strncmp (request, "CAPA", 4) == 0)
    {
      CAPA ();
    }
  else if (strncmp (request, "APOP", 4) == 0)
    {
      APOP (request);
    }
  else if (strncmp (request, "TOP", 3) == 0)
    {
      TOP (request);
    }
  else if (strncmp (request, "UIDL", 4) == 0)
    {
      UIDL (request);
    }
  else
    {
      reply (reply_err[ERR_NO_COMMAND]);
    }
}

void
POP3::reply (const char *data)
{
  if (!data)
    return;

  send (client_sockfd, data, strlen (data), 0);

  std::string str_data (data);
  str_data = str_data.substr (0, str_data.size () - 2);

  LogPrint (eLogDebug, "POP3session: reply: Reply stream: ", str_data);
}

void
POP3::USER (char *request)
{
  if (session_state != STATE_USER)
    {
      reply (reply_err[ERR_DENIED]);
      return;
    }
  /// User is identity public name
  std::string str_req (request);

  LogPrint (eLogDebug, "POP3session: USER: Request: ", request,
            ", size: ", str_req.size ());

  str_req.erase (0, 5);

  LogPrint (eLogDebug, "POP3session: USER: Request: ", request,
            ", size: ", str_req.size ());

  std::string user = str_req.substr (0, str_req.size () - 2);

  if (check_user (user))
    {
      session_state = STATE_PASS;
      auto res = format_response (reply_ok[OK_USER], user.c_str ());
      reply (res.c_str ());
    }
  else
    {
      auto res = format_response (reply_err[ERR_USER], user.c_str ());
      reply (res.c_str ());
    }
}

void
POP3::PASS (char *request)
{
  if (session_state != STATE_PASS)
    {
      reply (reply_err[ERR_DENIED]);
      return;
    }

  // ToDo: looks like we can keep pass hash in identity file
  //   for now ignored
  std::string str_req (request);

  if (check_pass (str_req.substr (5, str_req.size () - 5)))
    {
      // ToDo: lock mail directory
      session_state = STATE_TRANSACTION;
      emails = pbote::kademlia::email_worker.check_inbox ();
      reply (reply_ok[OK_LOCK]);
    }
  else
    {
      session_state = STATE_USER;
      reply (reply_err[ERR_PASS]);
    }
}

void
POP3::STAT ()
{
  if (session_state != STATE_TRANSACTION)
    {
      reply (reply_err[ERR_DENIED]);
      return;
    }

  size_t emails_size = 0;
  for (const auto &email : emails)
    emails_size += email->bytes ().size ();

  auto res
      = format_response (reply_ok[OK_STAT], emails.size (), emails_size);
  reply (res.c_str ());
}

void
POP3::LIST (char *request)
{
  if (session_state != STATE_TRANSACTION)
    {
      reply (reply_err[ERR_DENIED]);
      return;
    }

  size_t emails_size = 0;
  std::string mail_list;
  size_t email_counter = 1;

  for (const auto &email : emails)
    {
      if (email->deleted ())
        continue;

      size_t email_size = email->bytes ().size ();
      emails_size += email_size;
      mail_list += format_response (templates[TEMPLATE_LIST_ITEM],
                                    email_counter, email_size)
                   + "\n";
      email_counter++;
    }

  auto res
      = format_response (reply_ok[OK_LIST], emails.size (), emails_size)
        + mail_list + ".\r\n";

  reply (res.c_str ());
}

void
POP3::RETR (char *request)
{
  if (session_state != STATE_TRANSACTION)
    {
      reply (reply_err[ERR_DENIED]);
      return;
    }

  std::string req_str (request);
  LogPrint (eLogDebug, "POP3session: RETR: Request string: ", req_str);

  req_str.erase (0, 5);

  if (req_str.size () - 1 < 1)
    {
      LogPrint (eLogError, "POP3session: RETR: Request is too short");
      reply (reply_err[ERR_SIMP]);
      return;
    }

  std::replace (req_str.begin (), req_str.end (), '\n', ';');
  std::replace (req_str.begin (), req_str.end (), '\r', ';');
  std::string message_number = req_str.substr (0, req_str.find (';'));

  LogPrint (eLogDebug, "POP3session: RETR: Message number: ", message_number);

  size_t message_num_int = size_t (std::stoi (message_number));
  if (message_num_int > emails.size ())
    {
      LogPrint (eLogError, "POP3session: RETR: Message number is to high");
      reply (reply_err[ERR_NOT_FOUND]);
      return;
    }

  auto bytes = emails[message_num_int - 1]->bytes ();
  std::string res = format_response (reply_ok[OK_RETR], bytes.size ());
  res.append (bytes.begin (), bytes.end ());
  res.append (".\r\n");
  reply (res.c_str ());
}

void
POP3::DELE (char *request)
{
  if (session_state != STATE_TRANSACTION)
    {
      reply (reply_err[ERR_DENIED]);
      return;
    }

  std::string req_str (request);
  LogPrint (eLogDebug, "POP3session: DELE: Request string: ", req_str);

  req_str.erase (0, 5);

  // ToDo: validation
  int message_number = std::stoi (req_str) - 1;
  if (message_number < 0 && (size_t)message_number >= emails.size ())
    {
      auto response = format_response (reply_err[ERR_NOT_FOUND]);
      reply (response.c_str ());
      return;
    }

  /// On DELE step we can only mark message as deleted
  /// file deletion occurs only in phase UPDATE at step QUIT
  /// https://datatracker.ietf.org/doc/html/rfc1939#page-8
  if (emails[message_number]->deleted ())
    {
      auto response = format_response (reply_err[ERR_REMOVED], message_number + 1);
      reply (response.c_str ());
      return;
    }

  emails[message_number]->deleted (true);
  auto response = format_response (reply_ok[OK_DEL], message_number + 1);
  reply (response.c_str ());
}

void
POP3::NOOP ()
{
  if (session_state != STATE_TRANSACTION)
  {
    reply (reply_err[ERR_DENIED]);
    return;
  }
    
  reply (reply_ok[OK_SIMP]);
}

void
POP3::RSET ()
{
  if (session_state != STATE_TRANSACTION)
    {
      reply (reply_err[ERR_DENIED]);
      return;
    }

  // ToDo
  auto response = format_response (reply_ok[OK_MAILDROP], 0, 0);
  reply (response.c_str ());
}

void
POP3::QUIT ()
{
  if (session_state == STATE_TRANSACTION)
    {
      /// Now we can remove marked emails
      /// https://datatracker.ietf.org/doc/html/rfc1939#section-6
      for (const auto &email : emails)
        {
          if (email->deleted ())
            pbote::fs::Remove (email->filename ());
        }
    }

  session_state = STATE_QUIT;
  reply (reply_ok[OK_QUIT]);

  finish ();
}

/// Extension
void
POP3::CAPA ()
{
  std::string reply_str;

  for (auto line : capa_list)
    reply_str.append (line);

  reply (reply_str.c_str ());
}

void
POP3::APOP (char *request)
{
  // ToDo: looks like we can keep pass hash in identity file
  //   for now ignored
  if (strncmp (request, "APOP", 4) != 0)
    {
      reply (reply_err[ERR_DENIED]);
      return;
    }

  // ToDo
  LogPrint (eLogDebug, "POP3session: APOP: Login successfully");

  session_state = STATE_TRANSACTION;

  reply (reply_ok[OK_LOCK]);
}

void
POP3::TOP (char *request)
{
  if (session_state != STATE_TRANSACTION)
    {
      reply (reply_err[ERR_DENIED]);
      return;
    }

  reply (reply_ok[OK_TOP]);
  // ToDo
}

void
POP3::UIDL (char *request)
{
  if (session_state != STATE_TRANSACTION)
    {
      reply (reply_err[ERR_DENIED]);
      return;
    }

  std::string uidl_list;
  size_t email_counter = 1;

  for (const auto &email : emails)
    {
      std::string email_uid = email->field ("Message-ID");
      uidl_list += format_response (templates[TEMPLATE_UIDL_ITEM],
                                    email_counter, email_uid.c_str ())
                   + "\n";
      email_counter++;
    }

  auto res = format_response (reply_ok[OK_UIDL], emails.size ())
             + uidl_list + ".\r\n";

  reply (res.c_str ());
}

bool
POP3::check_user (const std::string &user)
{
  LogPrint (eLogDebug, "POP3session: check_user: user: ", user);

  if (pbote::context.identityByName (user))
    return true;

  return false;
}

bool
POP3::check_pass (const std::string &pass)
{
  auto clean_pass = pass.substr (0, pass.size () - 2);
  LogPrint (eLogDebug, "POP3session: check_pass: pass: ", clean_pass);
  // ToDo
  // if (pbote::context.recipient_exist(pass))
  return true;
  // return false;
}

} // namespace pop3
} // namespace bote
