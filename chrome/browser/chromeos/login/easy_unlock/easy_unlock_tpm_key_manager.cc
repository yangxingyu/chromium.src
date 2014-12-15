// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/chromeos/login/easy_unlock/easy_unlock_tpm_key_manager.h"

#include <cryptohi.h>

#include "base/base64.h"
#include "base/bind.h"
#include "base/location.h"
#include "base/logging.h"
#include "base/memory/ref_counted.h"
#include "base/prefs/pref_registry_simple.h"
#include "base/prefs/pref_service.h"
#include "base/prefs/scoped_user_pref_update.h"
#include "base/single_thread_task_runner.h"
#include "base/thread_task_runner_handle.h"
#include "base/threading/worker_pool.h"
#include "base/time/time.h"
#include "base/values.h"
#include "chrome/browser/browser_process.h"
#include "chrome/common/pref_names.h"
#include "content/public/browser/browser_thread.h"
#include "crypto/nss_util_internal.h"
#include "crypto/rsa_private_key.h"
#include "crypto/scoped_nss_types.h"

namespace {

// The modulus length for RSA keys used by easy sign-in.
const int kKeyModulusLength = 2048;

// Relays |GetSystemSlotOnIOThread| callback to |response_task_runner|.
void RunCallbackOnThreadRunner(
    const scoped_refptr<base::SingleThreadTaskRunner>& response_task_runner,
    const base::Callback<void(crypto::ScopedPK11Slot)>& callback,
    crypto::ScopedPK11Slot slot) {
  response_task_runner->PostTask(FROM_HERE,
                                 base::Bind(callback, base::Passed(&slot)));
}

// Gets TPM system slot. Must be called on IO thread.
// The callback wil be relayed to |response_task_runner|.
void GetSystemSlotOnIOThread(
    const scoped_refptr<base::SingleThreadTaskRunner>& response_task_runner,
    const base::Callback<void(crypto::ScopedPK11Slot)>& callback) {
  base::Callback<void(crypto::ScopedPK11Slot)> callback_on_origin_thread =
      base::Bind(&RunCallbackOnThreadRunner, response_task_runner, callback);

  crypto::ScopedPK11Slot system_slot =
      crypto::GetSystemNSSKeySlot(callback_on_origin_thread);
  if (system_slot)
    callback_on_origin_thread.Run(system_slot.Pass());
}

// Checks if a private RSA key associated with |public_key| can be found in
// |slot|.
// Must be called on a worker thread.
scoped_ptr<crypto::RSAPrivateKey> GetPrivateKeyOnWorkerThread(
    PK11SlotInfo* slot,
    const std::string& public_key) {
  const uint8* public_key_uint8 =
      reinterpret_cast<const uint8*>(public_key.data());
  std::vector<uint8> public_key_vector(
      public_key_uint8, public_key_uint8 + public_key.size());

  scoped_ptr<crypto::RSAPrivateKey> rsa_key(
      crypto::RSAPrivateKey::FindFromPublicKeyInfo(public_key_vector));
  if (!rsa_key || rsa_key->key()->pkcs11Slot != slot)
    return scoped_ptr<crypto::RSAPrivateKey>();
  return rsa_key.Pass();
}

// Signs |data| using a private key associated with |public_key| and stored in
// |slot|. Once the data is signed, callback is run on |response_task_runner|.
// In case of an error, the callback will be passed an empty string.
void SignDataOnWorkerThread(
    crypto::ScopedPK11Slot slot,
    const std::string& public_key,
    const std::string& data,
    const scoped_refptr<base::SingleThreadTaskRunner>& response_task_runner,
    const base::Callback<void(const std::string&)>& callback) {
  scoped_ptr<crypto::RSAPrivateKey> private_key(
      GetPrivateKeyOnWorkerThread(slot.get(), public_key));
  if (!private_key) {
    LOG(ERROR) << "Private key for signing data not found";
    response_task_runner->PostTask(FROM_HERE,
                                   base::Bind(callback, std::string()));
    return;
  }

  SECItem sign_result = {siBuffer, NULL, 0};
  if (SEC_SignData(&sign_result,
                   reinterpret_cast<const unsigned char*>(data.data()),
                   data.size(),
                   private_key->key(),
                   SEC_OID_PKCS1_SHA256_WITH_RSA_ENCRYPTION) != SECSuccess) {
    LOG(ERROR) << "Failed to sign data";
    response_task_runner->PostTask(FROM_HERE,
                                   base::Bind(callback, std::string()));
    return;
  }

  std::string signature(reinterpret_cast<const char*>(sign_result.data),
                        sign_result.len);
  response_task_runner->PostTask(FROM_HERE, base::Bind(callback, signature));
}

// Creates a RSA key pair in |slot|. When done, it runs |callback| with the
// created public key on |response_task_runner|.
// If |public_key| is not empty, a key pair will be created only if the private
// key associated with |public_key| does not exist in |slot|. Otherwise the
// callback will be run with |public_key|.
void CreateTpmKeyPairOnWorkerThread(
    crypto::ScopedPK11Slot slot,
    const std::string& public_key,
    const scoped_refptr<base::SingleThreadTaskRunner>& response_task_runner,
    const base::Callback<void(const std::string&)>& callback) {
  if (!public_key.empty() &&
      GetPrivateKeyOnWorkerThread(slot.get(), public_key)) {
    response_task_runner->PostTask(FROM_HERE, base::Bind(callback, public_key));
    return;
  }

  scoped_ptr<crypto::RSAPrivateKey> rsa_key(
      crypto::RSAPrivateKey::CreateSensitive(slot.get(), kKeyModulusLength));
  if (!rsa_key) {
    LOG(ERROR) << "Failed to create an RSA key.";
    response_task_runner->PostTask(FROM_HERE,
                                   base::Bind(callback, std::string()));
    return;
  }

  std::vector<uint8> created_public_key;
  if (!rsa_key->ExportPublicKey(&created_public_key)) {
    LOG(ERROR) << "Failed to export public key.";
    response_task_runner->PostTask(FROM_HERE,
                                   base::Bind(callback, std::string()));
    return;
  }

  response_task_runner->PostTask(
      FROM_HERE,
      base::Bind(callback,
                 std::string(created_public_key.begin(),
                             created_public_key.end())));
}

}  // namespace

// static
void EasyUnlockTpmKeyManager::RegisterLocalStatePrefs(
    PrefRegistrySimple* registry) {
  registry->RegisterDictionaryPref(prefs::kEasyUnlockLocalStateTpmKeys);
}

// static
void EasyUnlockTpmKeyManager::ResetLocalStateForUser(
    const std::string& user_id) {
  if (!g_browser_process)
    return;
  PrefService* local_state = g_browser_process->local_state();
  if (!local_state)
    return;

  DictionaryPrefUpdate update(local_state, prefs::kEasyUnlockLocalStateTpmKeys);
  update->RemoveWithoutPathExpansion(user_id, NULL);
}

EasyUnlockTpmKeyManager::EasyUnlockTpmKeyManager(const std::string& user_id,
                                                 PrefService* local_state)
    : user_id_(user_id),
      local_state_(local_state),
      create_tpm_key_state_(CREATE_TPM_KEY_NOT_STARTED),
      get_tpm_slot_weak_ptr_factory_(this),
      weak_ptr_factory_(this) {
}

EasyUnlockTpmKeyManager::~EasyUnlockTpmKeyManager() {
}

bool EasyUnlockTpmKeyManager::PrepareTpmKey(
    bool check_private_key,
    const base::Closure& callback) {
  CHECK(!user_id_.empty());

  if (create_tpm_key_state_ == CREATE_TPM_KEY_DONE)
    return true;

  std::string key = GetPublicTpmKey(user_id_);
  if (!check_private_key && !key.empty() &&
      create_tpm_key_state_ == CREATE_TPM_KEY_NOT_STARTED) {
    return true;
  }

  prepare_tpm_key_callbacks_.push_back(callback);

  if (create_tpm_key_state_ == CREATE_TPM_KEY_NOT_STARTED) {
    create_tpm_key_state_ = CREATE_TPM_KEY_WAITING_FOR_SYSTEM_SLOT;

    base::Callback<void(crypto::ScopedPK11Slot)> create_key_with_system_slot =
        base::Bind(&EasyUnlockTpmKeyManager::CreateKeyInSystemSlot,
                   get_tpm_slot_weak_ptr_factory_.GetWeakPtr(),
                   key);

    content::BrowserThread::PostTask(
        content::BrowserThread::IO,
        FROM_HERE,
        base::Bind(&GetSystemSlotOnIOThread,
                   base::ThreadTaskRunnerHandle::Get(),
                   create_key_with_system_slot));
  }

  return false;
}

bool EasyUnlockTpmKeyManager::StartGetSystemSlotTimeoutMs(size_t timeout_ms) {
  if (create_tpm_key_state_ == CREATE_TPM_KEY_DONE ||
      create_tpm_key_state_ == CREATE_TPM_KEY_GOT_SYSTEM_SLOT) {
    return false;
  }

  base::ThreadTaskRunnerHandle::Get()->PostDelayedTask(
      FROM_HERE,
      base::Bind(&EasyUnlockTpmKeyManager::OnTpmKeyCreated,
                 get_tpm_slot_weak_ptr_factory_.GetWeakPtr(),
                 std::string()),
      base::TimeDelta::FromMilliseconds(timeout_ms));
  return true;
}

std::string EasyUnlockTpmKeyManager::GetPublicTpmKey(
    const std::string& user_id) {
  if (!local_state_)
    return std::string();
  const base::DictionaryValue* dict =
      local_state_->GetDictionary(prefs::kEasyUnlockLocalStateTpmKeys);
  std::string key;
  if (dict)
    dict->GetStringWithoutPathExpansion(user_id, &key);
  std::string decoded;
  base::Base64Decode(key, &decoded);
  return decoded;
}

void EasyUnlockTpmKeyManager::SignUsingTpmKey(
    const std::string& user_id,
    const std::string& data,
    const base::Callback<void(const std::string& data)> callback) {
  std::string key = GetPublicTpmKey(user_id);
  if (key.empty()) {
    callback.Run(std::string());
    return;
  }

  base::Callback<void(crypto::ScopedPK11Slot)> sign_with_system_slot =
      base::Bind(&EasyUnlockTpmKeyManager::SignDataWithSystemSlot,
                 weak_ptr_factory_.GetWeakPtr(),
                 key, data, callback);

  content::BrowserThread::PostTask(
      content::BrowserThread::IO,
      FROM_HERE,
      base::Bind(&GetSystemSlotOnIOThread,
                 base::ThreadTaskRunnerHandle::Get(),
                 sign_with_system_slot));
}

void EasyUnlockTpmKeyManager::SetKeyInLocalState(const std::string& user_id,
                                                 const std::string& value) {
  if (!local_state_)
    return;

  std::string encoded;
  base::Base64Encode(value, &encoded);
  DictionaryPrefUpdate update(local_state_,
                              prefs::kEasyUnlockLocalStateTpmKeys);
  update->SetStringWithoutPathExpansion(user_id, encoded);
}

void EasyUnlockTpmKeyManager::CreateKeyInSystemSlot(
    const std::string& public_key,
    crypto::ScopedPK11Slot system_slot) {
  CHECK(system_slot);

  create_tpm_key_state_ = CREATE_TPM_KEY_GOT_SYSTEM_SLOT;

  // If there are any delayed tasks posted using |StartGetSystemSlotTimeoutMs|,
  // this will cancel them.
  // Note that this would cancel other pending |CreateKeyInSystemSlot| tasks,
  // but there should be at most one such task at a time.
  get_tpm_slot_weak_ptr_factory_.InvalidateWeakPtrs();

  base::WorkerPool::PostTask(
      FROM_HERE,
      base::Bind(&CreateTpmKeyPairOnWorkerThread,
                 base::Passed(&system_slot),
                 public_key,
                 base::ThreadTaskRunnerHandle::Get(),
                 base::Bind(&EasyUnlockTpmKeyManager::OnTpmKeyCreated,
                            weak_ptr_factory_.GetWeakPtr())),
      true /* long task */);
}

void EasyUnlockTpmKeyManager::SignDataWithSystemSlot(
    const std::string& public_key,
    const std::string& data,
    const base::Callback<void(const std::string& data)> callback,
    crypto::ScopedPK11Slot system_slot) {
  CHECK(system_slot);

  base::WorkerPool::PostTask(
      FROM_HERE,
      base::Bind(&SignDataOnWorkerThread,
                 base::Passed(&system_slot),
                 public_key,
                 data,
                 base::ThreadTaskRunnerHandle::Get(),
                 base::Bind(&EasyUnlockTpmKeyManager::OnDataSigned,
                            weak_ptr_factory_.GetWeakPtr(),
                            callback)),
      true /* long task */);
}

void EasyUnlockTpmKeyManager::OnTpmKeyCreated(const std::string& public_key) {
  // |OnTpmKeyCreated| is called by a timeout task posted by
  // |StartGetSystemSlotTimeoutMs|. Invalidating the factory will have
  // an effect of canceling any pending |GetSystemSlotOnIOThread| callbacks,
  // as well as other pending timeouts.
  // Note that in the case |OnTpmKeyCreated| was called as a result of
  // |CreateKeyInSystemSlot|, this should have no effect as no weak ptrs from
  // this factory should be in use in this case.
  get_tpm_slot_weak_ptr_factory_.InvalidateWeakPtrs();

  if (!public_key.empty())
    SetKeyInLocalState(user_id_, public_key);

  for (size_t i = 0; i < prepare_tpm_key_callbacks_.size(); ++i) {
    if (!prepare_tpm_key_callbacks_[i].is_null())
      prepare_tpm_key_callbacks_[i].Run();
  }

  prepare_tpm_key_callbacks_.clear();

  // If key creation failed, reset the state machine.
  create_tpm_key_state_ =
      public_key.empty() ? CREATE_TPM_KEY_NOT_STARTED : CREATE_TPM_KEY_DONE;
}

void EasyUnlockTpmKeyManager::OnDataSigned(
    const base::Callback<void(const std::string&)>& callback,
    const std::string& signature) {
  callback.Run(signature);
}
