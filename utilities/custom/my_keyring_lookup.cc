#include <cstring>   // for memcpy
#include "my_keyring_lookup.h"
#include "plugin/keyring/common/keys_container.h"

namespace {

/**
  Build a Key object with the given ID. 
  In InnoDB, the "key_id" might be "INNODBKey-<uuid>-<id>".
  Return a pointer that the caller can feed to fetch_key(...).
*/
keyring::IKey* create_temp_key_object(const std::string &key_id) {
    
  // arguments to the Key constructor are:
  //   (const char* key_id, const char* key_type, const char* user_id,
  //    const void* key_data, size_t key_len)
  // We'll pass empty strings for key_type/user_id, and no data.
  keyring::Key* tmp = new keyring::Key(
    key_id.c_str(),   // key_id
    "",               // key_type
    "",               // user_id
    nullptr, 0        // no data
  );
  return tmp;
}

} // end unnamed namespace

bool MyKeyringLookup::get_innodb_master_key(
    const std::string &srv_uuid,
    uint32_t master_key_id,
    std::vector<unsigned char> &out_key)
{
  // 1) Build the typical InnoDB key name: "INNODBKey-<uuid>-<master_key_id>"
  std::string key_name;
  if (!srv_uuid.empty()) {
    key_name = "INNODBKey-" + srv_uuid + "-" + std::to_string(master_key_id);
  } else {
    // fallback if you have no server uuid
    // In older MySQL, you might do: "INNODBKey-<server_id>-<master_key_id>"
    key_name = "INNODBKey-" + std::to_string(master_key_id);
  }

  // 2) Create a temporary Key object 
  std::unique_ptr<keyring::IKey> temp_key(create_temp_key_object(key_name));

  // 3) Use container->fetch_key(...). If successful, the container
  //    will fill temp_key with the raw data. If not found, it returns nullptr.
  keyring::IKey* fetched_ptr = m_keys->fetch_key(temp_key.get());
  if (!fetched_ptr) {
    std::cerr << "MyKeyringLookup: No such key in container: " << key_name << "\n";
    return false;
  }
  // fetched_ptr is the same pointer as temp_key.get(), but let's be explicit:
  // - if non-null, we now have the raw bytes in temp_key->get_key_data().

  // 4) Copy the raw data out
  size_t raw_len = fetched_ptr->get_key_data_size();   // key bytes length
  const unsigned char *raw_data = fetched_ptr->get_key_data();
  if (!raw_data || raw_len == 0) {
    std::cerr << "MyKeyringLookup: key " << key_name
              << " has no data in container?\n";
    return false;
  }

  out_key.resize(raw_len);
  std::memcpy(out_key.data(), raw_data, raw_len);

  std::cout << "MyKeyringLookup: Fetched " << raw_len
            << " bytes for key `" << key_name << "`\n";

  // 5) done, temp_key is freed automatically by unique_ptr destructor
  return true;
}
