/**
 * Copyright (C) 2019-2022, polistern
 *
 * This file is part of pboted and licensed under BSD3
 *
 * See full license text in LICENSE file at top of project tree
 */

#include <ctime>
#include <iterator>
#include <openssl/sha.h>
#include <utility>
#include <vector>

#include "BoteContext.h"
#include "DHTworker.h"
#include "EmailWorker.h"

namespace pbote
{
namespace kademlia
{

EmailWorker email_worker;

EmailWorker::EmailWorker ()
  : started_ (false),
    m_send_thread_ (nullptr),
    m_worker_thread_ (nullptr)
{
}

EmailWorker::~EmailWorker ()
{
  stop ();

  if (m_worker_thread_)
    {
      m_worker_thread_->join ();

      delete m_worker_thread_;
      m_worker_thread_ = nullptr;
    }
}

void
EmailWorker::start ()
{
  if (started_ && m_worker_thread_)
    return;

  if (context.get_identities_count () == 0)
    LogPrint (eLogError, "EmailWorker: Have no Bote identities for start");
  else
    {
      startSendEmailTask ();
      startCheckEmailTasks ();
      start_check_delivery_task ();
    }

  started_ = true;
  m_worker_thread_ = new std::thread ([this] { run (); });
}

void
EmailWorker::stop ()
{
  if (!started_)
    return;

  started_ = false;
  stopSendEmailTask ();
  stopCheckEmailTasks ();
  stop_check_delivery_task ();

  LogPrint (eLogInfo, "EmailWorker: Stopped");
}

void
EmailWorker::startCheckEmailTasks ()
{
  if (!started_ || !context.get_identities_count ())
    return;

  auto email_identities = context.getEmailIdentities ();
  // ToDo: move to object?
  for (const auto &identity : email_identities)
    {
      bool thread_exist = check_thread_exist (identity->publicName);
      if (thread_exist)
        continue;

      auto new_thread = std::make_shared<std::thread> (
          [this, identity] { checkEmailTask (identity); });

      LogPrint (eLogInfo, "EmailWorker: Start check task for ",
                identity->publicName);
      m_check_threads_[identity->publicName] = std::move (new_thread);
    }
}

bool
EmailWorker::stopCheckEmailTasks ()
{
  LogPrint (eLogInfo, "EmailWorker: Stopping check tasks");

  while (!m_check_threads_.empty ())
    {
      auto it = m_check_threads_.begin ();

      LogPrint (eLogInfo, "EmailWorker: Stopping task for ", it->first);
      it->second->join ();
      m_check_threads_.erase (it->first);
      
    }

  LogPrint (eLogInfo, "EmailWorker: Check tasks stopped");
  return true;
}

void
EmailWorker::startSendEmailTask ()
{
  if (!started_ || !context.get_identities_count ())
    return;

  LogPrint (eLogInfo, "EmailWorker: Start send task");
  m_send_thread_ = new std::thread ([this] { sendEmailTask (); });
}

bool
EmailWorker::stopSendEmailTask ()
{
  LogPrint (eLogInfo, "EmailWorker: Stopping send task");

  if (m_send_thread_ && !started_)
    {
      m_send_thread_->join ();

      delete m_send_thread_;
      m_send_thread_ = nullptr;
    }

  LogPrint (eLogInfo, "EmailWorker: Send task stopped");
  return true;
}

void
EmailWorker::start_check_delivery_task ()
{
  if (!started_)
    return;

  LogPrint (eLogInfo, "EmailWorker: Start check delivery task");
  m_check_thread_ = new std::thread ([this] { check_delivery_task (); });
}

bool
EmailWorker::stop_check_delivery_task ()
{
  LogPrint (eLogInfo, "EmailWorker: Stopping check delivery task");

  if (m_check_thread_ && !started_)
    {
      m_check_thread_->join ();

      delete m_check_thread_;
      m_check_thread_ = nullptr;
    }

  LogPrint (eLogInfo, "EmailWorker: Check delivery task stopped");
  return true;
}

void
EmailWorker::run ()
{
  while (started_)
    {
      size_t id_count = context.get_identities_count ();

      if (id_count)
        {
          LogPrint (eLogInfo, "EmailWorker: Identities now: ", id_count);
          startCheckEmailTasks ();

          if (!m_send_thread_)
            {
              LogPrint (eLogDebug, "EmailWorker: Try to start send task");
              startSendEmailTask ();
            }
        }
      else
        {
          LogPrint (eLogWarning, "EmailWorker: Have no identities for start");
          stopSendEmailTask ();
          stopCheckEmailTasks ();
        }

      std::this_thread::sleep_for (std::chrono::seconds (60));
    }
}

void
EmailWorker::checkEmailTask (const sp_id_full &email_identity)
{
  bool first_complete = false;
  std::string id_name = email_identity->publicName;
  while (started_)
    {
      // ToDo: read interval parameter from config
      if (first_complete)
        std::this_thread::sleep_for (
            std::chrono::seconds (CHECK_EMAIL_INTERVAL));
      first_complete = true;

      auto index_packets = retrieveIndex (email_identity);

      auto local_index_packet
          = DHT_worker.getIndex (email_identity->identity.GetIdentHash ());

      if (!local_index_packet.empty ())
        {
          LogPrint (eLogDebug, "EmailWorker: Check: ", id_name,
                    ": Got local index");

          /// from_net is true, because we save it as is
          IndexPacket parsed_local_index_packet;
          bool parsed = parsed_local_index_packet.fromBuffer (
              local_index_packet, true);

          if (parsed
              && parsed_local_index_packet.data.size ()
                     == parsed_local_index_packet.nump)
            {
              index_packets.push_back (parsed_local_index_packet);
            }
        }
      else
        {
          LogPrint (eLogDebug, "EmailWorker: Check: ", id_name,
                    ": Can't find local index");
        }

      LogPrint (eLogDebug, "EmailWorker: Check: ", id_name,
                ": Index count: ", index_packets.size ());

      if (index_packets.empty ())
        {
          LogPrint (eLogDebug, "EmailWorker: Check: ", id_name,
                    ": Have no indices");
          LogPrint (eLogInfo, "EmailWorker: Check: ", id_name,
                    ": Round complete");
          continue;
        }

      if (!started_)
        return;

      auto enc_mail_packets = retrieveEmail (index_packets);

      LogPrint (eLogDebug, "EmailWorker: Check: ", id_name,
                ": Mail count: ", enc_mail_packets.size ());

      if (enc_mail_packets.empty ())
        {
          LogPrint (eLogDebug, "EmailWorker: Check: ", id_name,
                    ": Have no mail for process");
          LogPrint (eLogInfo, "EmailWorker: Check: ", id_name,
                    ": Round complete");
          continue;
        }

      auto emails = processEmail (email_identity, enc_mail_packets);

      LogPrint (eLogInfo, "EmailWorker: Check: ", id_name,
                ": email(s) processed: ", emails.size ());

      // ToDo: check mail signature
      for (auto mail : emails)
        {
          mail.save ("inbox");

          EmailDeleteRequestPacket delete_email_packet;

          auto email_packet = mail.getDecrypted ();
          memcpy (delete_email_packet.DA, email_packet.DA, 32);
          auto enc_email_packet = mail.getEncrypted ();
          memcpy (delete_email_packet.key, enc_email_packet.key, 32);

          i2p::data::Tag<32> email_dht_key (enc_email_packet.key);
          i2p::data::Tag<32> email_del_auth (email_packet.DA);

          /// We need to remove packets for all received email from nodes
          // ToDo: multipart email support
          std::vector<std::string> responses;
          responses = DHT_worker.deleteEmail (email_dht_key,
                                              DataE, delete_email_packet);

          if (responses.empty ())
          {
            LogPrint (eLogInfo, "EmailWorker: Check: ", id_name,
                      ": Email not removed from DHT");
          }

          /// Same for Index packets
          // ToDo: multipart email support
          responses = DHT_worker.deleteIndexEntry (
                      email_identity->identity.GetIdentHash (), email_dht_key,
                      email_del_auth);

          if (responses.empty ())
          {
            LogPrint (eLogInfo, "EmailWorker: Check: ", id_name,
                      ": Index not removed from DHT");
          }
        }

      LogPrint (eLogInfo, "EmailWorker: Check: ", id_name, ": complete");
    }

  LogPrint (eLogInfo, "EmailWorker: Check: ", id_name, ": Stopped");
}

void
EmailWorker::incompleteEmailTask ()
{
  // ToDo: need to implement
  //   for multipart mail packets
}

void
EmailWorker::sendEmailTask ()
{
  v_sp_email outbox;
  while (started_)
    {
      // ToDo: read interval parameter from config
      std::this_thread::sleep_for (std::chrono::seconds (SEND_EMAIL_INTERVAL));

      std::vector<std::string> nodes;
      checkOutbox (outbox);

      if (outbox.empty ())
        {
          LogPrint (eLogDebug, "EmailWorker: Send: Outbox empty");
          continue;
        }

      /// Store Encrypted Email Packet
      for (const auto &email : outbox)
        {
          if (email->skip ())
            {
              LogPrint (eLogWarning, "EmailWorker: Send: Email skipped");
              continue;
            }

          // ToDo: Sign before encrypt
          //email->sign ();
          email->encrypt ();

          if (email->skip ())
            {
              LogPrint (eLogWarning, "EmailWorker: Send: Email skipped");
              continue;
            }

          StoreRequestPacket store_packet;

          auto encrypted_mail = email->getEncrypted ();
          store_packet.data = encrypted_mail.toByte ();
          store_packet.length = store_packet.data.size ();
          LogPrint (eLogDebug, "EmailWorker: Send: store_packet.length: ",
                    store_packet.length);

          /// For now, HashCash not checking from Java Bote side
          store_packet.hashcash = email->hashcash ();
          store_packet.hc_length = store_packet.hashcash.size ();
          LogPrint (eLogDebug, "EmailWorker: Send: store_packet.hc_length: ",
                    store_packet.hc_length);

          /// Send Store Request with Encrypted Email Packet to nodes
          i2p::data::Tag<32> email_dht_key (encrypted_mail.key);
          nodes = DHT_worker.store (email_dht_key, DataE, store_packet);

          /// If have no OK store responses - mark message as skipped
          if (nodes.empty ())
            {
              email->skip (true);
              LogPrint (eLogWarning, "EmailWorker: Send: email not sent");
              continue;
            }

          DHT_worker.safe (encrypted_mail.toByte ());
          LogPrint (eLogDebug, "EmailWorker: Send: Email sent to ",
                    nodes.size (), " node(s)");
        }

      /// Create and store Index Packet
      // ToDo: move to function?
      for (const auto &email : outbox)
        {
          if (email->skip ())
            continue;

          IndexPacket new_index_packet;

          auto recipient = email->get_recipient ();
          memcpy (new_index_packet.hash,
                  recipient->GetIdentHash ().data (), 32);

          // ToDo: for test, need to rewrite
          // for (const auto &email : encryptedEmailPackets) {
          IndexPacket::Entry entry;
          memcpy (entry.key, email->getEncrypted ().key, 32);
          memcpy (entry.dv, email->getEncrypted ().delete_hash, 32);
          entry.time = context.ts_now ();

          new_index_packet.data.push_back (entry);
          //}

          new_index_packet.nump = new_index_packet.data.size ();

          StoreRequestPacket store_index_packet;

          /// For now it's not checking from Java-Bote side
          store_index_packet.hashcash = email->hashcash ();
          store_index_packet.hc_length = store_index_packet.hashcash.size ();
          LogPrint (eLogDebug, "EmailWorker: Send: store_index.hc_length: ",
              store_index_packet.hc_length);

          auto index_bytes = new_index_packet.toByte ();

          store_index_packet.length = index_bytes.size ();
          store_index_packet.data = index_bytes;

          /// Send Store Request with Index Packet to nodes
          nodes = DHT_worker.store (recipient->GetIdentHash (), DataI,
                                    store_index_packet);

          /// If have no OK store responses - mark message as skipped
          if (nodes.empty ())
            {
              email->skip (true);
              LogPrint (eLogWarning, "EmailWorker: Send: Index not sent");
              continue;
            }

          DHT_worker.safe (new_index_packet.toByte ());
          LogPrint (eLogDebug, "EmailWorker: Send: Index send to ",
                    nodes.size (), " node(s)");
        }

      auto email_it = outbox.begin ();
      while (email_it != outbox.end ())
        {
          if ((*email_it)->skip ())
            {
              ++email_it;
              continue;
            }

          (*email_it)->setField ("X-I2PBote-Deleted", "false");
          /// Write new metadata before move file to sent
          (*email_it)->save ("");
          (*email_it)->move ("sent");
          email_it = outbox.erase(email_it);
          LogPrint (eLogInfo,
                    "EmailWorker: Send: Email sent, removed from outbox");
        }

      LogPrint (eLogInfo, "EmailWorker: Send: Round complete");
    }

  LogPrint (eLogInfo, "EmailWorker: Send: Stopped");
}

void
EmailWorker::check_delivery_task ()
{
  LogPrint (eLogInfo, "EmailWorker: Check delivery started");
  // Check Sent directory
  while (started_)
    {
      // If empty - sleep 5 min
      std::this_thread::sleep_for (std::chrono::seconds (CHECK_EMAIL_INTERVAL));

      // Read meta data from sent email

      // Make DeletionQuery to DHT
      // std::vector<std::shared_ptr<DeletionInfoPacket> > results;
      // results = DHT_worker.deletion_query (key);

      // Compare DeletionInfoPacket in results with Email meta (key, DA)

      // If have more than one valid DeletionInfoPacket - mark delivered

      LogPrint (eLogInfo, "EmailWorker: Check delivery: Round complete");
    }
}

std::vector<IndexPacket>
EmailWorker::retrieveIndex (const sp_id_full &identity)
{
  auto identity_hash = identity->identity.GetIdentHash ();
  LogPrint (eLogDebug, "EmailWorker: retrieveIndex: Try to find index for: ",
            identity_hash.ToBase64 ());
  /* Use findAll rather than findOne because some peers might have an
   *  incomplete set of Email Packet keys, and because we want to send
   *  IndexPacketDeleteRequests to all of them.
   */

  auto results = DHT_worker.findAll (identity_hash, DataI);
  if (results.empty ())
    {
      LogPrint (eLogWarning,
                "EmailWorker: retrieveIndex: Can't find index for: ",
                identity_hash.ToBase64 ());
      return {};
    }

  std::map<i2p::data::Tag<32>, IndexPacket> indices;
  /// Retrieve index packets
  for (const auto &response : results)
    {
      if (response->type != type::CommN)
        {
          // ToDo: looks like in case if we got request to ourself, for now we
          // just skip it
          LogPrint (eLogWarning, "EmailWorker: retrieveIndex: Got ",
                    "non-response packet in batch, type: ", response->type,
                    ", ver: ", unsigned (response->ver));
          continue;
        }

      LogPrint (eLogDebug, "EmailWorker: retrieveIndex: Got response from: ",
                response->from.substr (0, 15), "...");
      
      ResponsePacket res_packet;
      bool parsed = res_packet.from_comm_packet (*response, true);

      if (!parsed)
        {
          LogPrint (eLogDebug, "EmailWorker: retrieveIndex: ",
                    "Can't parse packet, skipped");
          continue;
        }

      if (res_packet.status != StatusCode::OK)
        {
          LogPrint (eLogWarning, "EmailWorker: retrieveIndex: Status: ",
                    statusToString (res_packet.status));
          continue;
        }

      if (res_packet.length < 38)
        {
          LogPrint (eLogDebug, "EmailWorker: retrieveIndex: ",
                    "Empty packet, skipped");
          continue;
        }

      if (DHT_worker.safe (res_packet.data))
        LogPrint (eLogDebug, "EmailWorker: retrieveIndex: Index packet saved");

      IndexPacket index_packet;
      parsed = index_packet.fromBuffer (res_packet.data, true);

      if (!parsed)
        {
          LogPrint (eLogDebug, "EmailWorker: retrieveIndex: ",
                    "Can't parse packet, skipped");
          continue;
        }

      if (index_packet.data.empty ())
        {
          LogPrint (eLogWarning, "EmailWorker: retrieveIndex: Empty packet");
          continue;
        }

      i2p::data::Tag<32> hash (index_packet.hash);
      indices.insert (std::pair<i2p::data::Tag<32>, IndexPacket> (hash, index_packet));
    }

  LogPrint (eLogDebug, "EmailWorker: retrieveIndex: Indices parsed: ",
            indices.size ());

  std::vector<IndexPacket> res;
  res.reserve (indices.size ());

  for (const auto &packet : indices)
    res.push_back (packet.second);

  // save index packets for interrupt case
  // ToDo: check if we have packet locally and sent delete request now

  return res;
}

std::vector<EmailEncryptedPacket>
EmailWorker::retrieveEmail (const std::vector<IndexPacket> &indices)
{
  std::vector<std::shared_ptr<CommunicationPacket> > responses;
  std::vector<EmailEncryptedPacket> local_email_packets;

  for (const auto &index : indices)
    {
      for (auto entry : index.data)
        {
          i2p::data::Tag<32> hash (entry.key);

          auto local_email_packet = DHT_worker.getEmail (hash);
          if (!local_email_packet.empty ())
            {
              LogPrint (eLogDebug,
                        "EmailWorker: retrieveEmail: Got local "
                        "encrypted email for key: ", hash.ToBase64 ());
              EmailEncryptedPacket parsed_local_email_packet;
              bool parsed = parsed_local_email_packet.fromBuffer (
                  local_email_packet.data (), local_email_packet.size (),
                  true);

              if (parsed && !parsed_local_email_packet.edata.empty ())
                {
                  local_email_packets.push_back (parsed_local_email_packet);
                }
            }
          else
            {
              LogPrint (eLogDebug,
                        "EmailWorker: retrieveEmail: Can't find local "
                        "encrypted email for key: ", hash.ToBase64 ());
            }

          auto temp_results = DHT_worker.findAll (hash, DataE);
          responses.insert (responses.end (), temp_results.begin (),
                            temp_results.end ());
        }
    }

  LogPrint (eLogDebug, "EmailWorker: retrieveEmail: Responses: ",
            responses.size ());

  std::map<i2p::data::Tag<32>, EmailEncryptedPacket> mail_packets;
  for (const auto &response : responses)
    {
      if (response->type != type::CommN)
        {
          // ToDo: looks like we got request to ourself, for now just skip it
          LogPrint (eLogWarning,
                    "EmailWorker: retrieveIndex: Got non-response packet in "
                    "batch, type: ", response->type, ", ver: ",
                    unsigned (response->ver));
          continue;
        }

      ResponsePacket res_packet;
      bool parsed = res_packet.from_comm_packet (*response, true);

      if (!parsed)
        {
          LogPrint (eLogDebug, "EmailWorker: retrieveEmail: ",
                    "Can't parse packet, skipped");
          continue;
        }

      if (res_packet.status != StatusCode::OK)
        {
          LogPrint (eLogWarning, "EmailWorker: retrieveEmail: Status: ",
                    statusToString (res_packet.status));
          continue;
        }

      if (res_packet.length <= 0)
        {
          LogPrint (eLogDebug, "EmailWorker: retrieveEmail: ",
                    "Empty packet, skipped");
          continue;
        }

      LogPrint (eLogDebug, "EmailWorker: retrieveEmail: Got email ",
                "packet, payload size: ", res_packet.length);

      if (DHT_worker.safe (res_packet.data))
        LogPrint (eLogDebug, "EmailWorker: retrieveEmail: Encrypted ",
                  "email packet saved locally");

      EmailEncryptedPacket email_packet;
      parsed = email_packet.fromBuffer (res_packet.data.data (),
                                        res_packet.length, true);

      if (!parsed || email_packet.edata.empty ())
        {
          LogPrint (eLogWarning, "EmailWorker: retrieveEmail: Mail packet",
                    " without entries");
          continue;
        }

      i2p::data::Tag<32> hash (email_packet.key);
      mail_packets.insert (std::pair<i2p::data::Tag<32>, 
                                     EmailEncryptedPacket> (hash, email_packet));
    }

  LogPrint (eLogDebug, "EmailWorker: retrieveEmail: Parsed mail ",
            "packets: ",mail_packets.size ());

  for (auto local_packet : local_email_packets)
    {
      i2p::data::Tag<32> hash (local_packet.key);
      mail_packets.insert (
          std::pair<i2p::data::Tag<32>, EmailEncryptedPacket> (
              hash, local_packet));
    }

  LogPrint (eLogDebug, "EmailWorker: retrieveEmail: Mail packets: ",
            mail_packets.size ());

  std::vector<EmailEncryptedPacket> res;
  res.reserve (mail_packets.size ());

  for (const auto &packet : mail_packets)
    res.push_back (packet.second);

  // save encrypted email packets for interrupt case
  // ToDo: check if we have packet locally and sent delete request now

  return res;
}

std::vector<EmailUnencryptedPacket>
EmailWorker::loadLocalIncompletePacket ()
{
  // ToDo: TBD
  // ToDo: move to ?
  /*std::string indexPacketPath = pbote::fs::DataDirPath("incomplete");
  std::vector<std::string> packets_path;
  std::vector<EmailUnencryptedPacket> indexPackets;
  auto result = pbote::fs::ReadDir(indexPacketPath, packets_path);
  if (result) {
    for (const auto &packet_path : packets_path) {
      std::ifstream file(packet_path, std::ios::binary);

      std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(file)),
  (std::istreambuf_iterator<char>()));

      file.close();
      auto indexPacket = parseEmailUnencryptedPkt(bytes.data(), bytes.size(),
  false); if (!indexPacket.data.empty()) indexPackets.push_back(indexPacket);
    }
    LogPrint(eLogDebug, "Email: loadLocalIndex: loaded index files: ",
  indexPackets.size()); return indexPackets;
  }
  LogPrint(eLogWarning, "Email: loadLocalIndex: have no index files");*/
  return {};
}

void
EmailWorker::checkOutbox (v_sp_email &emails)
{
  /// outbox contain plain text packets
  // ToDo: encrypt all local stored emails with master password
  std::string outboxPath = pbote::fs::DataDirPath ("outbox");
  std::vector<std::string> mails_path;
  auto result = pbote::fs::ReadDir (outboxPath, mails_path);

  if (!result)
    {
      LogPrint (eLogDebug, "EmailWorker: checkOutbox: No emails for sending");
      return;
    }

  for (const auto &mail : emails)
    {
      /// If we check outbox - we can try to re-send skipped emails
      mail->skip (false);

      auto path = std::find(mails_path.begin (), mails_path.end (), mail->filename ());
      if (path != std::end(mails_path))
        {
          LogPrint (eLogDebug, "EmailWorker: checkOutbox: Already in outbox: ",
                    mail->filename ());
          mails_path.erase (path);
        }
    }

  for (const auto &mail_path : mails_path)
    {

      /// Read mime packet
      std::ifstream file (mail_path, std::ios::binary);
      std::vector<uint8_t> bytes ((std::istreambuf_iterator<char> (file)),
                                  (std::istreambuf_iterator<char> ()));
      file.close ();

      Email mailPacket;
      mailPacket.fromMIME (bytes);

      if (mailPacket.length () > 0)
        LogPrint (eLogDebug,"EmailWorker: checkOutbox: loaded: ", mail_path);
      else
        {
          LogPrint (eLogWarning, "EmailWorker: checkOutbox: can't read: ", mail_path);
          continue;
        }

      mailPacket.filename (mail_path);

      /**
       * Check if FROM and TO fields have valid public names, else
       * Check if <name@domain> in AddressBook for replacement
       * if not found - log warning and skip
       * if replaced - save modified email to file to keep changes
       */
      std::string from_label = mailPacket.get_from_label ();
      std::string from_address = mailPacket.get_from_address ();
      std::string to_label = mailPacket.get_to_label();
      std::string to_address = mailPacket.get_to_addresses();

      LogPrint (eLogDebug,"EmailWorker: checkOutbox: from: ", from_label);
      LogPrint (eLogDebug,"EmailWorker: checkOutbox: from: ", from_address);
      LogPrint (eLogDebug,"EmailWorker: checkOutbox: to: ", to_label);
      LogPrint (eLogDebug,"EmailWorker: checkOutbox: to: ", to_address);

      /// First try to find our identity
      // ToDo: Anon send
      if (from_label.empty () || from_address.empty ())
        {
          LogPrint (eLogWarning, "EmailWorker: checkOutbox: FROM empty");
          continue;
        }

      auto label_from_identity = context.identityByName (from_label);
      auto address_from_identity = context.identityByName (from_address);

      if (label_from_identity)
        mailPacket.set_sender_identity(label_from_identity);
      else if (address_from_identity)
        mailPacket.set_sender_identity(address_from_identity);
      else
        {
          LogPrint (eLogError, "EmailWorker: checkOutbox: Unknown, label: ",
                    from_label, ", address: ", from_address);
          mailPacket.set_sender_identity(nullptr);
          continue;
        }

      // Now we can try to set correct TO field
      if (to_label.empty () || to_address.empty ())
        {
          LogPrint (eLogWarning, "EmailWorker: checkOutbox: TO empty");
          continue;
        }

      std::string old_to_address = mailPacket.field("To");

      auto label_to_address = context.address_for_name (to_label);
      auto address_to_address = context.address_for_alias (to_address);

      std::string new_to, b_dest;

      if (!label_to_address.empty ())
        {
          new_to.append (to_label + " <" + label_to_address + ">");
          b_dest = label_to_address;
        }
      else if (!address_to_address.empty ())
        {
          new_to.append (to_label + " <" + address_to_address + ">");
          b_dest = address_to_address;
        }
      else
        {
          LogPrint (eLogWarning, "EmailWorker: checkOutbox: Can't find ",
                    to_address, ", try to use as is");
          to_address = mailPacket.get_to_mailbox ();
          new_to.append (to_label + " <" + to_address + ">");
          b_dest = to_address;
        }

      LogPrint (eLogDebug,"EmailWorker: checkOutbox: TO replaced, old: ",
                old_to_address, ", new: ", new_to);

      mailPacket.set_to (new_to);
      mailPacket.set_recipient_identity(b_dest);

      if (mailPacket.skip ())
        {
          LogPrint (eLogDebug,"EmailWorker: checkOutbox: Email skipped");
          continue;
        }

      /// On this step will be generated Message-ID and
      ///   it will be saved and not be re-generated
      ///   on the next loading (if first attempt failed)
      mailPacket.compose ();
      mailPacket.save ("");
      mailPacket.bytes ();

      auto recipient = mailPacket.get_recipient ();

      if (!recipient)
        {
          LogPrint (eLogError,"EmailWorker: checkOutbox: Recipient error");
          continue;
        }

      if (recipient->GetKeyType () == KEY_TYPE_X25519_ED25519_SHA512_AES256CBC)
        mailPacket.compress (Email::CompressionAlgorithm::ZLIB);
      else
        mailPacket.compress (Email::CompressionAlgorithm::UNCOMPRESSED);

      // ToDo: slice big packet after compress

      if (!mailPacket.empty ())
        emails.push_back (std::make_shared<Email> (mailPacket));
    }

  LogPrint (eLogInfo, "EmailWorker: checkOutbox: Got ", emails.size (),
            " email(s)");
}

v_sp_email
EmailWorker::check_inbox ()
{
  // ToDo: encrypt all local stored emails
  std::string outboxPath = pbote::fs::DataDirPath ("inbox");
  std::vector<std::string> mails_path;
  auto result = pbote::fs::ReadDir (outboxPath, mails_path);

  v_sp_email emails;

  if (result)
    {
      for (const auto &mail_path : mails_path)
        {
          /// Read mime packet
          std::ifstream file (mail_path, std::ios::binary);
          std::vector<uint8_t> bytes ((std::istreambuf_iterator<char> (file)),
                                      (std::istreambuf_iterator<char> ()));
          file.close ();

          Email mailPacket;
          mailPacket.fromMIME (bytes);

          if (mailPacket.length () > 0)
            {
              LogPrint (eLogDebug, "EmailWorker: check_inbox: File loaded: ",
                        mail_path);
            }
          else
            {
              LogPrint (eLogWarning,
                        "EmailWorker: check_inbox: Can't read file: ",
                        mail_path);
              continue;
            }

          // ToDo: check signature and set header field

          mailPacket.compose ();
          mailPacket.filename (mail_path);

          if (!mailPacket.empty ())
            emails.push_back (std::make_shared<Email> (mailPacket));
        }
    }

  LogPrint (eLogDebug, "EmailWorker: check_inbox: Found ", emails.size (),
            " email(s).");

  return emails;
}

std::vector<Email>
EmailWorker::processEmail (
    const sp_id_full &identity,
    const std::vector<EmailEncryptedPacket> &mail_packets)
{
  // ToDo: move to incompleteEmailTask?
  LogPrint (eLogDebug, "EmailWorker: processEmail: Emails for process: ",
            mail_packets.size ());
  std::vector<Email> emails;

  for (auto enc_mail : mail_packets)
    {
      std::vector<uint8_t> unencrypted_email_data;

      if (enc_mail.edata.empty ())
        {
          LogPrint (eLogWarning, "EmailWorker: processEmail: Packet is empty ");
          continue;
        }

      unencrypted_email_data = identity->identity.Decrypt (
          enc_mail.edata.data (), enc_mail.edata.size ());

      if (unencrypted_email_data.empty ())
        {
          LogPrint (eLogWarning, "EmailWorker: processEmail: Can't decrypt ");
          continue;
        }

      Email temp_mail (unencrypted_email_data, true);

      if (!temp_mail.verify (enc_mail.delete_hash))
        {
          i2p::data::Tag<32> cur_hash (enc_mail.delete_hash);
          LogPrint (eLogWarning, "EmailWorker: processEmail: email ",
                    cur_hash.ToBase64 (), " is unequal");
          continue;
        }

      temp_mail.setEncrypted (enc_mail);

      if (!temp_mail.empty ())
        emails.push_back (temp_mail);
    }

  LogPrint (eLogDebug,
            "EmailWorker: processEmail: Emails processed: ", emails.size ());

  return emails;
}

bool
EmailWorker::check_thread_exist (const std::string &identity_name)
{
  auto it = m_check_threads_.find (identity_name);
  if (it != m_check_threads_.end ())
    return true;

  return false;
}

} // namespace kademlia
} // namespace pbote
