/**
 * Copyright (C) 2019-2022, polistern
 *
 * This file is part of pboted and licensed under BSD3
 *
 * See full license text in LICENSE file at top of project tree
 */

#ifndef BOTE_CONTEXT_H__
#define BOTE_CONTEXT_H__

#include <chrono>
#include <random>

#include "AddressBook.h"
#include "BoteIdentity.h"
#include "ConfigParser.h"
#include "FileSystem.h"
#include "Logging.h"
#include "Packet.h"

#include "Identity.h"

namespace pbote
{

#define DEFAULT_KEY_FILE_NAME "destination.key"

using queue_type = std::shared_ptr<pbote::util::Queue<std::shared_ptr<PacketForQueue>>>;

class BoteContext
{
 public:
  BoteContext();
  ~BoteContext();

  void init();

  void send(const PacketForQueue& packet);
  void send(const std::shared_ptr<PacketBatch<pbote::CommunicationPacket>>& batch);

  bool receive(const std::shared_ptr<pbote::CommunicationPacket>& packet);

  void removeBatch(const std::shared_ptr<PacketBatch<pbote::CommunicationPacket>>& batch);

  std::string get_nickname() { return nickname; }

  std::string get_listen_host() { return listenHost; }
  uint16_t get_listen_port_SAM() { return listenPortSAM; }

  std::string get_router_host() { return routerHost; }
  uint16_t get_router_port_TCP() { return routerPortTCP; }
  uint16_t get_router_port_UDP() { return routerPortUDP; }

  std::shared_ptr<i2p::data::IdentityEx> getLocalDestination() { return localDestination; }
  std::shared_ptr<i2p::data::PrivateKeys> getlocalKeys() { return local_keys_; }

  size_t get_identities_count() { return getEmailIdentities().size(); }
  std::shared_ptr<pbote::BoteIdentityFull> identityByName(const std::string &name);
  std::vector<std::shared_ptr<pbote::BoteIdentityFull>> getEmailIdentities() { return identities_storage_->getIdentities(); };

  bool name_exist(const std::string &name) { return address_book_.name_exist(name); }
  bool alias_exist(const std::string &alias) { return address_book_.alias_exist(alias); }
  std::string address_for_name(const std::string &name) { return address_book_.address_for_name(name); }
  std::string address_for_alias(const std::string &alias) { return address_book_.address_for_alias(alias); }

  queue_type getSendQueue() { return m_sendQueue; }
  queue_type getRecvQueue() { return m_recvQueue; }

  int32_t get_uptime();
  unsigned long get_bytes_recv() { return bytes_recv_; }
  unsigned long get_bytes_sent() { return bytes_sent_; }
  bool keys_loaded() { return keys_loaded_; }

  void save_new_keys(std::shared_ptr<i2p::data::PrivateKeys> localKeys);

  void add_recv_byte_count(unsigned long byte_count) { bytes_recv_ += byte_count; };
  void add_sent_byte_count(unsigned long byte_count) { bytes_sent_ += byte_count; };
  void random_cid(uint8_t *buf, size_t len);
  int32_t ts_now ();

 private:
  int readLocalIdentity(const std::string &path);
  void saveLocalIdentity(const std::string &path);

  bool keys_loaded_;

  std::string listenHost;
  uint16_t listenPortSAM;
  std::string nickname;
  std::string routerHost;
  uint16_t routerPortTCP;
  uint16_t routerPortUDP;

  identitiesStorage *identities_storage_;

  pbote::AddressBook address_book_;

  uint64_t start_time_;
  uint64_t bytes_recv_;
  uint64_t bytes_sent_;

  queue_type m_recvQueue;
  queue_type m_sendQueue;

  std::shared_ptr<i2p::data::IdentityEx> localDestination;
  std::shared_ptr<i2p::data::PrivateKeys> local_keys_;

  std::vector<std::shared_ptr<PacketBatch<pbote::CommunicationPacket>>> runningBatches;

  std::independent_bits_engine<std::default_random_engine, CHAR_BIT, uint8_t> rbe;
};

extern BoteContext context;

} // namespace pbote

#endif
