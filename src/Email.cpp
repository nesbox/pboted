/**
 * Copyright (C) 2019-2022, polistern
 *
 * This file is part of pboted and licensed under BSD3
 *
 * See full license text in LICENSE file at top of project tree
 */

#include <cassert>
#include <iostream>
#include <fstream>
#include <cstdio>

#include "BoteContext.h"
#include "Email.h"

// libi2pd
#include "Gzip.h"

namespace pbote
{

void *
_lzmaAlloc (ISzAllocPtr, size_t size)
{
  return new uint8_t[size];
}

void
_lzmaFree (ISzAllocPtr, void *addr)
{
  if (!addr)
    return;

  delete[] reinterpret_cast<uint8_t *> (addr);
}

ISzAlloc _allocFuncs
= {
   _lzmaAlloc, _lzmaFree
};

Email::Email ()
  : incomplete_ (false),
    empty_ (true),
    skip_ (false),
    deleted_ (false)
{
}

Email::Email (const std::vector<uint8_t> &data, bool from_net)
  : skip_(false),
    deleted_(false)
{
  // ToDo: Move to function fromPacket
  LogPrint (eLogDebug, "Email: Payload size: ", data.size ());
  /// 72 because type[1] + ver[1] + mes_id[32] + DA[32] + fr_id[2] + fr_count[2] + length[2]
  if (data.size() < 72)
    {
      LogPrint(eLogWarning, "Email: Payload is too short");
    }

  size_t offset = 0;

  std::memcpy(&packet.type, data.data(), 1);
  offset += 1;
  std::memcpy(&packet.ver, data.data() + offset, 1);
  offset += 1;

  if (packet.type != (uint8_t) 'U')
    {
      LogPrint(eLogWarning, "Email: Wrong type: ", packet.type);
    }

  if (packet.ver != (uint8_t) 4)
    {
      LogPrint(eLogWarning, "Email: Wrong version: ", unsigned(packet.ver));
    }

  std::memcpy (&packet.mes_id, data.data() + offset, 32);
  offset += 32;
  std::memcpy(&packet.DA, data.data() + offset, 32);
  offset += 32;
  std::memcpy(&packet.fr_id, data.data() + offset, 2);
  offset += 2;
  std::memcpy(&packet.fr_count, data.data() + offset, 2);
  offset += 2;
  std::memcpy(&packet.length, data.data() + offset, 2);
  offset += 2;

  // ToDo: use it
  i2p::data::Tag<32> mes_id(packet.mes_id);
  LogPrint(eLogDebug, "Email: mes_id: ", mes_id.ToBase64());

  if (from_net)
    {
      packet.fr_id = ntohs(packet.fr_id);
      packet.fr_count = ntohs(packet.fr_count);
      packet.length = ntohs(packet.length);
    }

  LogPrint(eLogDebug, "Email: fr_id: ", packet.fr_id, ", fr_count: ",
           packet.fr_count, ", length: ", packet.length);

  if (packet.fr_id >= packet.fr_count)
    {
      LogPrint(eLogError, "Email: Illegal values, fr_id: ", packet.fr_id,
               ", fr_count: ", packet.fr_count);
    }

  incomplete_ = packet.fr_id + 1 != packet.fr_count;
  empty_ = packet.length == 0;
  packet.data = std::vector<uint8_t> (data.data() + offset, data.data() + data.size());
  decompress (packet.data);
  fromMIME (packet.data);
}

void
Email::fromMIME (const std::vector<uint8_t> &email_data)
{
  std::string message(email_data.begin(), email_data.end());
  mail.load(message.begin(), message.end());

  for (const auto &entity : mail.header())
    {
      auto it = std::find(HEADER_WHITELIST.begin(), HEADER_WHITELIST.end(), entity.name());
      if (it != HEADER_WHITELIST.end())
        LogPrint(eLogDebug, "Email: fromMIME: ", entity.name(), ": ", entity.value());
      else
        {
          mail.header().field(entity.name()).value("");
          LogPrint(eLogDebug, "Email: fromMIME: Forbidden header ", entity.name(), " removed");
        }
    }

  empty_ = false;
  packet.data = email_data;
  compose ();
}

void
Email::set_message_id ()
{
  std::string message_id = field ("Message-ID");
  if (!message_id.empty ())
    return;

  message_id = generate_uuid_v4 ();
  message_id.append ("@bote.i2p");
  setField ("Message-ID", message_id);
}

std::string
Email::get_message_id ()
{
  std::string message_id = field ("Message-ID");

  if (message_id.empty () ||
      (message_id.size () == 36 &&
       message_id.c_str ()[14] != 4))
    {
      LogPrint (eLogDebug, "Email: get_message_id: message ID is not 4 version or empty");
      set_message_id ();
      message_id = field ("Message-ID");
    }

  return message_id;
}

void
Email::set_message_id_bytes ()
{
  std::string message_id = get_message_id ();
  std::vector<uint8_t> res;
  /// Example
  /// 27d92c57-0503-4dd6-9bb3-fa2d0613855f
  const bool dash[]
    = { 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };

  for (int i = 0; i < 36; i++)
    {
      if (dash[i])
        continue;

      res.push_back (message_id.c_str ()[i]);
    }

  memcpy (packet.mes_id, res.data(), 32);
}

std::vector<uint8_t>
Email::hashcash ()
{
  /**
   * Format:
   * version: (currently 1)
   * bits: the number of leading bits that are 0
   * timestamp: a date/time stamp (time is optional)
   * resource: the data string being transmitted, for example, an IP address, email address, or other data
   * extension: ignored in version 1
   * random seed: base-64 encoded random set of characters
   * counter: base-64 encoded binary counter between 0 and 220, (1,048,576)
   *
   * Example:
   * 1:20:1303030600:admin@example.com::McMybZIhxKXu57jd:FOvXX
   */

  /*
  uint8_t version = 1;
  uint8_t bits = 20;
  std::string resource ("admin@example.com"), extension, seed, counter;

  const int32_t ts_now = context.ts_now ();
  // ToDo: think about it
  seed = std::string("McMybZIhxKXu57jd");
  counter = std::string("FOvXX");

  std::string hc_s;
  hc_s.append ("" + version);
  hc_s.append (":" + bits);
  hc_s.append (":" + ts_now);
  hc_s.append (":" + resource);
  hc_s.append (":" + extension);
  hc_s.append (":" + seed);
  hc_s.append (":" + counter);
  */

  // ToDo: temp, TBD
  std::string hc_s ("1:20:1303030600:admin@example.com::McMybZIhxKXu57jd:FOvXX");
  LogPrint (eLogDebug, "Email: hashcash: hashcash: ", hc_s);
  std::vector<uint8_t> result (hc_s.begin(), hc_s.end());

  return result;
}

std::string
Email::get_from_label ()
{
  return mail.header().from().begin()->label();
}

std::string
Email::get_from_mailbox ()
{
  return mail.header().from().begin()->mailbox();
}

std::string
Email::get_from_address ()
{
  auto mailbox = mail.header().from().begin()->mailbox();
  auto domain = mail.header().from().begin()->domain();
  return mailbox + "@" + domain;
}

std::string
Email::get_to_label ()
{
  return mail.header().to().begin()->mailbox().label();
}

std::string
Email::get_to_mailbox ()
{
  return mail.header().to().begin()->mailbox().mailbox();
}

std::string
Email::get_to_addresses ()
{
  auto mailbox = mail.header().to().begin()->mailbox().mailbox();
  auto domain = mail.header().to().begin()->mailbox().domain();
  return mailbox + "@" + domain;
}

bool
Email::verify (uint8_t *hash)
{
  uint8_t da_h[32] = {0};
  SHA256 (packet.DA, 32, da_h);
  //* For debug
  i2p::data::Tag<32> ver_hash (hash), cur_da (packet.DA), cur_hash (da_h);

  LogPrint(eLogDebug, "Email: verify: DV hash: ", ver_hash.ToBase64());
  LogPrint(eLogDebug, "Email: verify: DA curr: ", cur_da.ToBase64());
  LogPrint(eLogDebug, "Email: verify: DA hash: ", cur_hash.ToBase64());

  if (ver_hash != cur_hash)
    LogPrint (eLogError, "Email: verify: Hashes mismatch");
  //*/

  return memcmp(hash, da_h, 32) == 0;
}

std::vector<uint8_t>
Email::bytes ()
{
  std::stringstream buffer;
  buffer << mail;

  std::string str_buf = buffer.str ();
  std::vector<uint8_t> result (str_buf.begin (), str_buf.end ());

  packet.data = result;
  packet.length = result.size ();

  return result;
}

bool
Email::save (const std::string &dir)
{
  std::string emailPacketPath;
  // If email not loaded from file system, and we need to save it first time
  if (!dir.empty () && filename ().empty ())
    {
      emailPacketPath = pbote::fs::DataDirPath (dir, get_message_id () + ".mail");

      if (pbote::fs::Exists (emailPacketPath))
        {
          return false;
        }
    }
  else
    {
      emailPacketPath = filename ();
    }

  LogPrint (eLogDebug, "Email: save: save packet to ", emailPacketPath);

  std::ofstream file (emailPacketPath, std::ofstream::binary | std::ofstream::out);

  if (!file.is_open ())
    {
      LogPrint(eLogError, "Email: save: can't open file ", emailPacketPath);
      return false;
    }

  auto message_bytes = bytes ();

  file.write (reinterpret_cast<const char *> (message_bytes.data ()), message_bytes.size ());
  file.close ();

  return true;
}

bool
Email::move (const std::string &dir)
{
  if (skip ())
    return false;

  std::string new_path = pbote::fs::DataDirPath (dir, field ("X-I2PBote-DHT-Key") + ".mail");

  LogPrint (eLogDebug, "Email: move: old path: ", filename ());
  LogPrint (eLogDebug, "Email: move: new path: ", new_path);

  std::ifstream ifs (filename (), std::ios::in | std::ios::binary);
  std::ofstream ofs (new_path, std::ios::out | std::ios::binary);

  ofs << ifs.rdbuf ();

  int status = std::remove (filename ().c_str ());

  if (status == 0)
    {
      LogPrint (eLogInfo, "Email: move: File ", filename (), " moved to ", new_path);
      filename (new_path);

      return true;
    }

  LogPrint (eLogError, "Email: move: Can't move file ", filename (), " to ", new_path);

  return false;
}

void
Email::compose ()
{
  set_message_id ();
  set_message_id_bytes ();

  bytes ();

  LogPrint (eLogDebug, "Email: compose: Message-ID: ", get_message_id ());
  LogPrint (eLogDebug, "Email: compose: Message-ID bytes: ",
            get_message_id_bytes ().ToBase64 ());

  uint8_t zero_array[32] = {0};

  if (memcmp(packet.DA, zero_array, 32) == 0)
    context.random_cid (packet.DA, 32);

  i2p::data::Tag<32> del_auth (packet.DA);
  LogPrint (eLogDebug, "Email: compose: Message DA: ", del_auth.ToBase64 ());

  // ToDo
  packet.fr_id = 0;
  packet.fr_count = 1;
  packet.length = packet.data.size ();

  empty_ = false;
  incomplete_ = false;

  /// For debug only
  std::stringstream buffer;
  buffer << mail;
  LogPrint (eLogDebug, "Email: compose: content:\n", buffer.str ());
  ///
}

void
Email::encrypt ()
{
  if (skip ())
    return;

  if (encrypted_)
    return;

  SHA256 (packet.DA, 32, encrypted.delete_hash);
  /// For debug only
  i2p::data::Tag<32> del_hash (encrypted.delete_hash), del_auth (packet.DA);
  LogPrint (eLogDebug, "Email: encrypt: del_auth: ", del_auth.ToBase64 ());
  LogPrint (eLogDebug, "Email: encrypt: del_hash: ", del_hash.ToBase64 ());
  ///

  setField ("X-I2PBote-Delete-Auth-Hash", del_hash.ToBase64 ());

  LogPrint (eLogDebug, "Email: encrypt: packet.data.size: ", packet.data.size ());

  auto packet_bytes = packet.toByte ();

  if (!sender)
    {
      LogPrint (eLogError, "Email: encrypt: Sender error");
      skip (true);
      return;
    }

  encrypted.edata = sender->GetPublicIdentity ()->Encrypt (
          packet_bytes.data (), packet_bytes.size (),
          recipient->GetCryptoPublicKey ());

  if (encrypted.edata.empty ())
    {
      LogPrint (eLogError, "Email: encrypt: Encrypted data is empty, skipped");
      skip (true);
      return;
    }

  encrypted.length = encrypted.edata.size ();
  encrypted.alg = sender->GetKeyType ();
  encrypted.stored_time = 0;

  LogPrint (eLogDebug, "Email: encrypt: encrypted.edata.size(): ",
            encrypted.edata.size ());

  /// Get hash of data + length for DHT key
  const size_t data_for_hash_len = 2 + encrypted.edata.size ();
  std::vector<uint8_t> data_for_hash
      = { static_cast<uint8_t> (encrypted.length >> 8),
          static_cast<uint8_t> (encrypted.length & 0xff) };
  data_for_hash.insert (data_for_hash.end (), encrypted.edata.begin (), encrypted.edata.end ());

  SHA256 (data_for_hash.data (), data_for_hash_len, encrypted.key);

  i2p::data::Tag<32> dht_key (encrypted.key);

  setField ("X-I2PBote-DHT-Key", dht_key.ToBase64 ());

  LogPrint (eLogDebug, "Email: encrypt: dht_key: ", dht_key.ToBase64 ());
  LogPrint (eLogDebug, "Email: encrypt: encrypted.length : ", encrypted.length);

  encrypted_ = true;
}

bool
Email::compress (CompressionAlgorithm type)
{
  LogPrint (eLogDebug, "Email: compress: alg: ", unsigned (type));

  if (type == CompressionAlgorithm::LZMA)
    {
      LogPrint (eLogWarning, "Email: compress: We not support compression LZMA, will be uncompressed");
      type = CompressionAlgorithm::UNCOMPRESSED;
    }

  if (type == CompressionAlgorithm::ZLIB)
    {
      LogPrint (eLogDebug, "Email: compress: ZLIB, start compress");

      std::vector<uint8_t> output;
      zlibCompress (output, packet.data);

      packet.data.push_back (uint8_t (CompressionAlgorithm::ZLIB));
      packet.data.insert (packet.data.end (), output.begin (), output.end ());
      LogPrint (eLogDebug, "Email: compress: ZLIB compressed");
      return true;
    }

  if (type == CompressionAlgorithm::UNCOMPRESSED)
    {
      LogPrint (eLogDebug, "Email: compress: data uncompressed, save as is");
      packet.data.insert (packet.data.begin (),
                          (uint8_t) CompressionAlgorithm::UNCOMPRESSED);
      return true;
    }

  LogPrint (eLogWarning, "Email: compress: Unknown compress algorithm");

  return false;
}

void
Email::decompress (std::vector<uint8_t> v_mail)
{
  size_t offset = 0;
  uint8_t compress_alg;
  memcpy (&compress_alg, v_mail.data () + offset, 1);
  offset += 1;

  LogPrint (eLogDebug, "Email: decompress: compress alg: ", unsigned (compress_alg));

  if (compress_alg == CompressionAlgorithm::LZMA)
    {
      LogPrint (eLogDebug, "Email: decompress: LZMA compressed, start decompress");
      std::vector<uint8_t> output;
      lzmaDecompress (output,
                      std::vector<uint8_t>(v_mail.data() + offset,
                                           v_mail.data() + v_mail.size()));
      packet.data = output;
      LogPrint (eLogDebug, "Email: compress: LZMA decompressed");
      return;
    }

  if (compress_alg == CompressionAlgorithm::ZLIB)
    {
      LogPrint (eLogDebug, "Email: decompress: ZLIB compressed, start decompress");
      std::vector<uint8_t> output;
      zlibDecompress (output,
                      std::vector<uint8_t>(v_mail.data() + offset,
                                           v_mail.data() + v_mail.size()));
      packet.data = output;
      LogPrint (eLogDebug, "Email: compress: ZLIB decompressed");
      return;
    }

  if (compress_alg == CompressionAlgorithm::UNCOMPRESSED)
    {
      LogPrint (eLogDebug, "Email: decompress: data uncompressed, save as is");
      packet.data = std::vector<uint8_t> (v_mail.begin () + 1, v_mail.end ());
      return;
    }

  LogPrint(eLogWarning, "Email: decompress: Unknown compress algorithm, try to save as is");
  packet.data = std::vector<uint8_t>(v_mail.begin() + 1, v_mail.end());
}

std::string
Email::generate_uuid_v4 ()
{
  static std::random_device              rd;
  static std::mt19937                    gen (rd ());
  static std::uniform_int_distribution<> dis (0, 15);
  static std::uniform_int_distribution<> dis2 (8, 11);

  std::stringstream ss;
  int i;

  ss << std::hex;
  for (i = 0; i < 8; i++)
    ss << dis (gen);

  ss << "-";
  for (i = 0; i < 4; i++)
    ss << dis (gen);

  ss << "-4";
  for (i = 0; i < 3; i++)
    ss << dis (gen);

  ss << "-";
  ss << dis2 (gen);
  for (i = 0; i < 3; i++)
    ss << dis (gen);

  ss << "-";
  for (i = 0; i < 12; i++)
    ss << dis (gen);

  return ss.str ();
}

void
Email::lzmaDecompress (std::vector<uint8_t> &outBuf,
                       const std::vector<uint8_t> &inBuf)
{
  CLzmaDec dec;

  LzmaDec_Construct (&dec);
  SRes res = LzmaDec_Allocate (&dec, &inBuf[0], LZMA_PROPS_SIZE, &_allocFuncs);
  assert (res == SZ_OK);

  LzmaDec_Init (&dec);

  unsigned outPos = 0, inPos = LZMA_PROPS_SIZE;
  ELzmaStatus status;
  const size_t BUF_SIZE = 10240;
  outBuf.resize (25 * 1024 * 1024);

  while (outPos < outBuf.size ())
    {
      SizeT destLen = std::min (BUF_SIZE, outBuf.size () - outPos);
      SizeT srcLen = std::min (BUF_SIZE, inBuf.size () - inPos);

      res = LzmaDec_DecodeToBuf (&dec,
                                 &outBuf[outPos], &destLen,
                                 &inBuf[inPos], &srcLen,
                                 (outPos + destLen == outBuf.size ())
                                 ? LZMA_FINISH_END : LZMA_FINISH_ANY, &status);
      assert (res == SZ_OK);
      inPos += srcLen;
      outPos += destLen;
      if (status == LZMA_STATUS_FINISHED_WITH_MARK)
        {
          LogPrint (eLogDebug, "Email: lzmaDecompress: Finished with mark");
          break;
        }
    }

  LzmaDec_Free (&dec, &_allocFuncs);
  outBuf.resize (outPos);
}

void
Email::zlibCompress (std::vector<uint8_t> &outBuf,
                     const std::vector<uint8_t> &inBuf)
{
  i2p::data::GzipInflator inflator;
  inflator.Inflate (inBuf.data (), inBuf.size (),
                    outBuf.data (), outBuf.size ());
}

void
Email::zlibDecompress (std::vector<uint8_t> &outBuf,
                       const std::vector<uint8_t> &inBuf)
{
  i2p::data::GzipDeflator deflator;
  deflator.Deflate (inBuf.data (), inBuf.size (),
                    outBuf.data (), outBuf.size ());
}

void
Email::set_sender_identity (sp_id_full identity)
{
  if (!identity)
    {
      LogPrint (eLogWarning, "Email: set_sender: Can't set "
                "sender identity, skipped");
      skip (true);
      return;
    }

  sender = std::make_shared<BoteIdentityPrivate>(identity->identity);
  std::string addr = sender->GetPublicIdentity ()->ToBase64v1 ();

  std::string old_from_address = field("From");
  std::string new_from;
  new_from.append (identity->publicName + " <b64." + addr + ">");

  set_from (new_from);
  set_sender (new_from);

  LogPrint (eLogDebug, "EmailWorker: set_sender: FROM replaced, old: ",
                old_from_address, ", new: ", new_from);

  LogPrint (eLogDebug, "Email: set_sender: sender: ", sender->ToBase64 ());
  LogPrint (eLogDebug, "Email: set_sender: email: sender hash: ",
            sender->GetIdentHash ().ToBase64 ());
}

void
Email::set_recipient_identity (std::string to_address)
{
  LogPrint (eLogDebug, "Email: set_recipient: to_address: ", to_address);

  std::string format_prefix = to_address.substr(0, to_address.find(".") + 1);

  if (format_prefix.compare(ADDRESS_B32_PREFIX) == 0)
    recipient = parse_address_v1(to_address);
  else if (format_prefix.compare(ADDRESS_B64_PREFIX) == 0)
    recipient = parse_address_v1(to_address);
  else
    recipient = parse_address_v0(to_address);

  if (recipient == nullptr)
    {
      LogPrint (eLogWarning, "Email: set_recipient: Can't create "
                "recipient from \"TO\" header, skip mail");
      skip (true);

      return;
    }

  LogPrint (eLogDebug, "Email: set_recipient: recipient: ",
            recipient->ToBase64 ());
  LogPrint (eLogDebug, "Email: set_recipient: recipient hash: ",
            recipient->GetIdentHash ().ToBase64 ());
}

sp_id_public
Email::parse_address_v0(std::string address)
{
  BoteIdentityPublic identity;
  size_t base64_key_len = 0, offset = 0;

  if (address.length() == ECDH256_ECDSA256_PUBLIC_BASE64_LENGTH)
    {
      identity = BoteIdentityPublic(KEY_TYPE_ECDH256_ECDSA256_SHA256_AES256CBC);
      base64_key_len = ECDH256_ECDSA256_PUBLIC_BASE64_LENGTH / 2;
    }
  else if (address.length() == ECDH521_ECDSA521_PUBLIC_BASE64_LENGTH)
    {
      identity = BoteIdentityPublic(KEY_TYPE_ECDH521_ECDSA521_SHA512_AES256CBC);
      base64_key_len = ECDH521_ECDSA521_PUBLIC_BASE64_LENGTH / 2;
    }
  else
    {
      LogPrint(eLogWarning, "EmailWorker: parse_address_v0: Unsupported identity type");
      return nullptr;
    }

  // Restore keys
  std::string cryptoPublicKey = "A" + address.substr(offset, (base64_key_len));
  offset += (base64_key_len);
  std::string signingPublicKey = "A" + address.substr(offset, (base64_key_len));

  std::string restored_identity_str;
  restored_identity_str.append(cryptoPublicKey);
  restored_identity_str.append(signingPublicKey);

  identity.FromBase64(restored_identity_str);

  LogPrint(eLogDebug, "EmailWorker: parse_address_v0: identity.ToBase64: ",
           identity.ToBase64());
  LogPrint(eLogDebug, "EmailWorker: parse_address_v0: idenhash.ToBase64: ",
           identity.GetIdentHash().ToBase64());

  return std::make_shared<BoteIdentityPublic>(identity);
}

sp_id_public
Email::parse_address_v1(std::string address)
{
  BoteIdentityPublic identity;
  std::string format_prefix = address.substr (0, address.find (".") + 1);
  std::string base_str = address.substr (format_prefix.length ());
  // ToDo: Define length from base32/64
  uint8_t identity_bytes[2048];
  size_t identity_len = 0;

  if (format_prefix.compare (ADDRESS_B32_PREFIX) == 0)
    identity_len = i2p::data::Base32ToByteStream (base_str.c_str (), base_str.length (), identity_bytes, 2048);
  else if (format_prefix.compare (ADDRESS_B64_PREFIX) == 0)
    identity_len = i2p::data::Base64ToByteStream (base_str.c_str (), base_str.length (), identity_bytes, 2048);
  else
    return nullptr;

  if (identity_len < 5)
    {
      LogPrint (eLogError, "identitiesStorage: parse_identity_v1: Malformed address");
      return nullptr;
    }

  if (identity_bytes[0] != ADDRES_FORMAT_V1)
    {
      LogPrint (eLogError, "identitiesStorage: parse_identity_v1: Unsupported address format");
      return nullptr;
    }

  if (identity_bytes[1] == CRYP_TYPE_ECDH256 &&
      identity_bytes[2] == SIGN_TYPE_ECDSA256 &&
      identity_bytes[3] == SYMM_TYPE_AES_256 &&
      identity_bytes[4] == HASH_TYPE_SHA_256)
    {
      identity = BoteIdentityPublic(KEY_TYPE_ECDH256_ECDSA256_SHA256_AES256CBC);
    }
  else if (identity_bytes[1] == CRYP_TYPE_ECDH521 &&
           identity_bytes[2] == SIGN_TYPE_ECDSA521 &&
           identity_bytes[3] == SYMM_TYPE_AES_256 &&
           identity_bytes[4] == HASH_TYPE_SHA_512)
    {
      identity = BoteIdentityPublic(KEY_TYPE_ECDH521_ECDSA521_SHA512_AES256CBC);
    }
  else if (identity_bytes[1] == CRYP_TYPE_X25519 &&
           identity_bytes[2] == SIGN_TYPE_ED25519 &&
           identity_bytes[3] == SYMM_TYPE_AES_256 &&
           identity_bytes[4] == HASH_TYPE_SHA_512)
    {
      identity = BoteIdentityPublic(KEY_TYPE_X25519_ED25519_SHA512_AES256CBC);
    }

  size_t len = identity.FromBuffer(identity_bytes + 5, identity_len);

  if (len == 0)
    return nullptr;

  LogPrint(eLogDebug, "identitiesStorage: parse_identity_v1: identity.ToBase64: ",
           identity.ToBase64());
  LogPrint(eLogDebug, "identitiesStorage: parse_identity_v1: idenhash.ToBase64: ",
           identity.GetIdentHash().ToBase64());

  return std::make_shared<BoteIdentityPublic>(identity);
}

} // pbote
