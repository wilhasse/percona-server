#pragma once

#include <string>
#include <vector>
#include <memory>
#include <iostream>

// Forward-declare Keys_container and Key
namespace keyring {
class Keys_container;
class IKey;
}  // namespace keyring

/**
  Simple helper that wraps a Keys_container to fetch a key by name.

  Example usage:
    MyKeyringLookup helper(keys_container_ptr);
    std::vector<unsigned char> master_key;
    if (!helper.get_innodb_master_key("server-uuid", 7, master_key)) {
      // error
    }
*/
class MyKeyringLookup {
public:
  /// Takes a pointer to your existing Keys_container
  explicit MyKeyringLookup(keyring::Keys_container* keys)
    : m_keys(keys)
  { }

  /**
    Build "INNODBKey-<srv_uuid>-<master_key_id>", fetch from container, 
    and copy raw bytes into out_key. 
    @return true on success, false if not found or error.
  */
  bool get_innodb_master_key(const std::string &srv_uuid,
                             uint32_t master_key_id,
                             std::vector<unsigned char> &out_key);

private:
  keyring::Keys_container* m_keys;
};

