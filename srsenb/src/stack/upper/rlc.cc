/**
 *
 * \section COPYRIGHT
 *
 * Copyright 2013-2020 Software Radio Systems Limited
 *
 * By using this file, you agree to the terms and conditions set
 * forth in the LICENSE file which can be found at the top level of
 * the distribution.
 *
 */

#include "srsenb/hdr/stack/upper/rlc.h"
#include "srsenb/hdr/stack/upper/common_enb.h"

namespace srsenb {

void rlc::init(pdcp_interface_rlc*    pdcp_,
               rrc_interface_rlc*     rrc_,
               mac_interface_rlc*     mac_,
               srslte::timer_handler* timers_,
               srslte::log_ref        log_h_)
{
  pdcp   = pdcp_;
  rrc    = rrc_;
  log_h  = log_h_;
  mac    = mac_;
  timers = timers_;

  pool = srslte::byte_buffer_pool::get_instance();

  pthread_rwlock_init(&rwlock, nullptr);
}

void rlc::stop()
{
  pthread_rwlock_wrlock(&rwlock);
  for (auto& user : users) {
    user.second.rlc->stop();
  }
  users.clear();
  pthread_rwlock_unlock(&rwlock);
  pthread_rwlock_destroy(&rwlock);
}

void rlc::get_metrics(rlc_metrics_t& m, const uint32_t nof_tti)
{
  m.ues.resize(users.size());
  size_t count = 0;
  for (auto& user : users) {
    user.second.rlc->get_metrics(m.ues[count], nof_tti);
    count++;
  }
}

void rlc::add_user(uint16_t rnti)
{
  pthread_rwlock_rdlock(&rwlock);
  if (users.count(rnti) == 0) {
    std::unique_ptr<srslte::rlc> obj(new srslte::rlc(log_h->get_service_name().c_str()));
    obj->init(&users[rnti],
              &users[rnti],
              timers,
              RB_ID_SRB0,
              [rnti, this](uint32_t lcid, uint32_t tx_queue, uint32_t retx_queue) {
                update_bsr(rnti, lcid, tx_queue, retx_queue);
              });
    users[rnti].rnti   = rnti;
    users[rnti].pdcp   = pdcp;
    users[rnti].rrc    = rrc;
    users[rnti].rlc    = std::move(obj);
    users[rnti].parent = this;
  }
  pthread_rwlock_unlock(&rwlock);
}

void rlc::rem_user(uint16_t rnti)
{
  pthread_rwlock_wrlock(&rwlock);
  if (users.count(rnti)) {
    users[rnti].rlc->stop();
    users.erase(rnti);
  } else {
    log_h->error("Removing rnti=0x%x. Already removed\n", rnti);
  }
  pthread_rwlock_unlock(&rwlock);
}

void rlc::clear_buffer(uint16_t rnti)
{
  pthread_rwlock_rdlock(&rwlock);
  if (users.count(rnti)) {
    users[rnti].rlc->empty_queue();
    for (int i = 0; i < SRSLTE_N_RADIO_BEARERS; i++) {
      if (users[rnti].rlc->has_bearer(i)) {
        mac->rlc_buffer_state(rnti, i, 0, 0);
      }
    }
    log_h->info("Cleared buffer rnti=0x%x\n", rnti);
  }
  pthread_rwlock_unlock(&rwlock);
}

void rlc::add_bearer(uint16_t rnti, uint32_t lcid, srslte::rlc_config_t cnfg)
{
  pthread_rwlock_rdlock(&rwlock);
  if (users.count(rnti)) {
    users[rnti].rlc->add_bearer(lcid, cnfg);
  }
  pthread_rwlock_unlock(&rwlock);
}

void rlc::add_bearer_mrb(uint16_t rnti, uint32_t lcid)
{
  pthread_rwlock_rdlock(&rwlock);
  if (users.count(rnti)) {
    users[rnti].rlc->add_bearer_mrb(lcid);
  }
  pthread_rwlock_unlock(&rwlock);
}

bool rlc::has_bearer(uint16_t rnti, uint32_t lcid)
{
  pthread_rwlock_rdlock(&rwlock);
  bool result = false;
  if (users.count(rnti)) {
    result = users[rnti].rlc->has_bearer(lcid);
  }
  pthread_rwlock_unlock(&rwlock);
  return result;
}

void rlc::del_bearer(uint16_t rnti, uint32_t lcid)
{
  pthread_rwlock_rdlock(&rwlock);
  if (users.count(rnti)) {
    users[rnti].rlc->del_bearer(lcid);
  }
  pthread_rwlock_unlock(&rwlock);
}

bool rlc::suspend_bearer(uint16_t rnti, uint32_t lcid)
{
  pthread_rwlock_rdlock(&rwlock);
  bool result = false;
  if (users.count(rnti)) {
    users[rnti].rlc->suspend_bearer(lcid);
    result = true;
  }
  pthread_rwlock_unlock(&rwlock);
  return result;
}

bool rlc::resume_bearer(uint16_t rnti, uint32_t lcid)
{
  pthread_rwlock_rdlock(&rwlock);
  bool result = false;
  if (users.count(rnti)) {
    users[rnti].rlc->resume_bearer(lcid);
    result = true;
  }
  pthread_rwlock_unlock(&rwlock);
  return result;
}

void rlc::reestablish(uint16_t rnti)
{
  pthread_rwlock_rdlock(&rwlock);
  if (users.count(rnti)) {
    users[rnti].rlc->reestablish();
  }
  pthread_rwlock_unlock(&rwlock);
}

// In the eNodeB, there is no polling for buffer state from the scheduler.
// This function is called by UE RLC instance every time the tx/retx buffers are updated
void rlc::update_bsr(uint32_t rnti, uint32_t lcid, uint32_t tx_queue, uint32_t retx_queue)
{
  log_h->debug("Buffer state: rnti=0x%x, lcid=%d, tx_queue=%d\n", rnti, lcid, tx_queue);
  mac->rlc_buffer_state(rnti, lcid, tx_queue, retx_queue);
}

void rlc::read_pdu_pcch(uint8_t* payload, uint32_t buffer_size)
{
  rrc->read_pdu_pcch(payload, buffer_size);
}

int rlc::read_pdu(uint16_t rnti, uint32_t lcid, uint8_t* payload, uint32_t nof_bytes)
{
  int ret;

  pthread_rwlock_rdlock(&rwlock);
  if (users.count(rnti)) {
    if (rnti != SRSLTE_MRNTI) {
      ret = users[rnti].rlc->read_pdu(lcid, payload, nof_bytes);
    } else {
      ret = users[rnti].rlc->read_pdu_mch(lcid, payload, nof_bytes);
    }
  } else {
    ret = SRSLTE_ERROR;
  }
  pthread_rwlock_unlock(&rwlock);
  return ret;
}

void rlc::write_pdu(uint16_t rnti, uint32_t lcid, uint8_t* payload, uint32_t nof_bytes)
{
  pthread_rwlock_rdlock(&rwlock);
  if (users.count(rnti)) {
    users[rnti].rlc->write_pdu(lcid, payload, nof_bytes);
  }
  pthread_rwlock_unlock(&rwlock);
}

void rlc::write_sdu(uint16_t rnti, uint32_t lcid, srslte::unique_byte_buffer_t sdu)
{
  pthread_rwlock_rdlock(&rwlock);
  if (users.count(rnti)) {
    if (rnti != SRSLTE_MRNTI) {
      users[rnti].rlc->write_sdu(lcid, std::move(sdu));
    } else {
      users[rnti].rlc->write_sdu_mch(lcid, std::move(sdu));
    }
  }
  pthread_rwlock_unlock(&rwlock);
}

void rlc::discard_sdu(uint16_t rnti, uint32_t lcid, uint32_t discard_sn)
{
  pthread_rwlock_rdlock(&rwlock);
  if (users.count(rnti)) {
    users[rnti].rlc->discard_sdu(lcid, discard_sn);
  }
  pthread_rwlock_unlock(&rwlock);
}

bool rlc::rb_is_um(uint16_t rnti, uint32_t lcid)
{
  bool ret = false;
  pthread_rwlock_rdlock(&rwlock);
  if (users.count(rnti)) {
    ret = users[rnti].rlc->rb_is_um(lcid);
  }
  pthread_rwlock_unlock(&rwlock);
  return ret;
}

bool rlc::sdu_queue_is_full(uint16_t rnti, uint32_t lcid)
{
  bool ret = false;
  pthread_rwlock_rdlock(&rwlock);
  if (users.count(rnti)) {
    ret = users[rnti].rlc->sdu_queue_is_full(lcid);
  }
  pthread_rwlock_unlock(&rwlock);
  return ret;
}

void rlc::user_interface::max_retx_attempted()
{
  rrc->max_retx_attempted(rnti);
}

void rlc::user_interface::write_pdu(uint32_t lcid, srslte::unique_byte_buffer_t sdu)
{
  if (lcid == RB_ID_SRB0) {
    rrc->write_pdu(rnti, lcid, std::move(sdu));
  } else {
    pdcp->write_pdu(rnti, lcid, std::move(sdu));
  }
}

void rlc::user_interface::write_pdu_bcch_bch(srslte::unique_byte_buffer_t sdu)
{
  ERROR("Error: Received BCCH from ue=%d\n", rnti);
}

void rlc::user_interface::write_pdu_bcch_dlsch(srslte::unique_byte_buffer_t sdu)
{
  ERROR("Error: Received BCCH from ue=%d\n", rnti);
}

void rlc::user_interface::write_pdu_pcch(srslte::unique_byte_buffer_t sdu)
{
  ERROR("Error: Received PCCH from ue=%d\n", rnti);
}

std::string rlc::user_interface::get_rb_name(uint32_t lcid)
{
  return to_string((rb_id_t)lcid);
}

} // namespace srsenb
