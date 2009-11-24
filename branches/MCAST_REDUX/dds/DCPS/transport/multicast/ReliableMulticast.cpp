/*
 * $Id$
 *
 * Copyright 2009 Object Computing, Inc.
 *
 * Distributed under the OpenDDS License.
 * See: http://www.opendds.org/license.html
 */

#include "ReliableMulticast.h"
#include "MulticastTransport.h"

#include "dds/DCPS/Serializer.h"

namespace OpenDDS {
namespace DCPS {

NakWatchdog::NakWatchdog(ReliableMulticast* link)
  : DataLinkWatchdog<ReliableMulticast>(link)
{
}

ACE_Time_Value
NakWatchdog::next_interval()
{
  MulticastConfiguration* config = this->link_->config();
  return config->nak_interval_;
}

void
NakWatchdog::on_interval(const void* /*arg*/)
{
  // Expire outstanding NAK requests that have not yet been
  // fulfilled; this prevents NAK implosions due to remote
  // peers becoming unreachable.
  this->link_->expire_naks();

  // Initiate resends by broadcasting MULTICAST_NAK control
  // messages to remote peers from which we are missing data:
  this->link_->send_naks();
}


SynWatchdog::SynWatchdog(ReliableMulticast* link)
  : DataLinkWatchdog<ReliableMulticast>(link)
{
}

ACE_Time_Value
SynWatchdog::next_interval()
{
  MulticastConfiguration* config = this->link_->config();
  return config->syn_interval_;
}

void
SynWatchdog::on_interval(const void* /*arg*/)
{
  // Initiate handshake by broadcasting MULTICAST_SYN control
  // messages to the remote peer assigned when the DataLink
  // was created:
  this->link_->send_syn();
}

ACE_Time_Value
SynWatchdog::next_timeout()
{
  MulticastConfiguration* config = this->link_->config();
  return config->syn_timeout_;
}

void
SynWatchdog::on_timeout(const void* /*arg*/)
{
  // There is no recourse if a link is unable to handshake; log
  // an error and return:
  ACE_ERROR((LM_ERROR,
             ACE_TEXT("(%P|%t) ERROR: ")
             ACE_TEXT("SynWatchdog::on_timeout: ")
             ACE_TEXT("timed out handshaking with remote peer: 0x%x!\n"),
             this->link_->remote_peer()));
}


ReliableMulticast::ReliableMulticast(MulticastTransport* transport,
                                     MulticastPeer local_peer,
                                     MulticastPeer remote_peer)
  : MulticastDataLink(transport,
                      local_peer,
                      remote_peer),
    acked_(false),
    nak_watchdog_(this),
    syn_watchdog_(this)
{
}

bool
ReliableMulticast::header_received(const TransportHeader& header)
{
  this->recvd_header_ = header;

  SequenceMap::iterator it(this->sequences_.find(header.source_));
  if (it == this->sequences_.end()) return true;  // unknown peer
  
  // Update last seen sequence for remote peer; return false if we
  // have already seen this packet to prevent duplicate delivery:
  return it->second.update(header.sequence_);
}

void
ReliableMulticast::sample_received(ReceivedDataSample& sample)
{
  switch(sample.header_.message_id_) {
  case TRANSPORT_CONTROL: {
    ACE_Message_Block* message = sample.sample_;

    switch (sample.header_.submessage_id_) {
    case MULTICAST_SYN:
      syn_received(message);
      break;

    case MULTICAST_SYNACK:
      synack_received(message);
      break;

    case MULTICAST_NAK:
      nak_received(message);
      break;

    case MULTICAST_NAKACK:
      nakack_received(message);
      break;

    default:
      ACE_ERROR((LM_WARNING,
                 ACE_TEXT("(%P|%t) WARNING: ")
                 ACE_TEXT("ReliableMulticast::sample_received: ")
                 ACE_TEXT("unknown TRANSPORT_CONTROL submessage: 0x%x!\n"),
                 sample.header_.submessage_id_));
    }
  } break;

  case SAMPLE_ACK:
    ack_received(sample);
    break;

  default:
    data_received(sample);
  }
}

bool
ReliableMulticast::acked()
{
  return this->acked_;
}

void
ReliableMulticast::expire_naks()
{
  ACE_Time_Value deadline(ACE_OS::gettimeofday());
  deadline -= this->config_->nak_timeout_;

  NakHistory::iterator first(this->nak_history_.begin());
  NakHistory::iterator last(this->nak_history_.upper_bound(deadline));
  
  if (first == last) return; // nothing to expire
  
  for (NakHistory::iterator it(first); it != last; ++it) {
    NakRequest& nak_request(it->second);
    
    SequenceMap::iterator sequence = this->sequences_.find(nak_request.first);
    if (sequence == this->sequences_.end()) {
      ACE_ERROR((LM_ERROR,
                 ACE_TEXT("(%P|%t) ERROR: ")
                 ACE_TEXT("ReliableMulticast::expire_naks: ")
                 ACE_TEXT("failed to find sequence for remote peer: 0x%x!\n"),
                 nak_request.first));
      continue;
    }

    // Skip undeliverable datagrams; attempt to establish a new
    // baseline to check for future reception gaps during delivery:
    if (nak_request.second > sequence->second) {
      ACE_ERROR((LM_WARNING,
                 ACE_TEXT("(%P|%t) WARNING: ")
                 ACE_TEXT("ReliableMulticast::expire_naks: ")
                 ACE_TEXT("skipping %d datagrams from unreliable remote peer: 0x%x!\n"),
                 sequence->second.depth(),
                 nak_request.first));
      
      sequence->second.skip(nak_request.second);
    }
  }
 
  // Remove expired repair requests:
  this->nak_history_.erase(first, last);
}

void
ReliableMulticast::send_naks()
{
  ACE_Time_Value now(ACE_OS::gettimeofday());

  for (SequenceMap::iterator it(this->sequences_.begin());
       it != this->sequences_.end(); ++it) {
    
    if (!it->second.disjoint()) continue; // nothing to NAK

    // Track high-water mark for peer on this interval; this value
    // is used to reset the low-water mark in the event a remote
    // peer does not respond to our repair requests:
    this->nak_history_.insert(NakHistory::value_type(
      now, NakRequest(it->first, it->second.high())));

    for (DisjointSequence::range_iterator range(it->second.range_begin());
         range != it->second.range_end(); ++range) {
      // Send MULTICAST_NAK request to remote peer. The peer should
      // respond with either a resend of the missing data or a
      // MULTICAST_NAKACK indicating the data is no longer available.
      send_nak(it->first, range->first, range->second);
    }
  }
}

void
ReliableMulticast::syn_received(ACE_Message_Block* message)
{
  TAO::DCPS::Serializer serializer(
    message, this->transport_->swap_bytes());

  MulticastPeer remote_peer;  // sent as local_peer
  MulticastPeer local_peer;   // sent as remote_peer

  serializer >> remote_peer;
  serializer >> local_peer;

  // Ignore message if not destined for us:
  if (local_peer != this->local_peer_) return;

  // Insert remote peer into sequence map. This establishes a baseline
  // to check for reception gaps during delivery of sample data:
  this->sequences_.insert(SequenceMap::value_type(
    remote_peer, DisjointSequence(this->recvd_header_.sequence_)));

  // MULTICAST_SYN control messages are always positively
  // acknowledged by a matching remote peer:
  send_synack(remote_peer);
}

void
ReliableMulticast::send_syn()
{
  size_t len = sizeof (this->local_peer_)
             + sizeof (this->remote_peer_);

  ACE_Message_Block* data;
  ACE_NEW_NORETURN(data, ACE_Message_Block(len));
  if (data == 0) {
    ACE_ERROR((LM_ERROR,
               ACE_TEXT("(%P|%t) ERROR: ")
               ACE_TEXT("ReliableMulticast::send_syn: ")
               ACE_TEXT("failed to allocate message!\n")));
    return;
  }

  TAO::DCPS::Serializer serializer(
    data, this->transport_->swap_bytes());

  serializer << this->local_peer_;
  serializer << this->remote_peer_;

  ACE_Message_Block* message =
    create_control(MULTICAST_SYN, data);

  int error;
  if ((error = send_control(message)) != SEND_CONTROL_OK) {
    ACE_ERROR((LM_ERROR,
               ACE_TEXT("(%P|%t) ERROR: ")
               ACE_TEXT("ReliableMulticast::send_syn: ")
               ACE_TEXT("send_control failed: %d!\n"),
               error));
    return;
  }
}

void
ReliableMulticast::synack_received(ACE_Message_Block* message)
{
  TAO::DCPS::Serializer serializer(
    message, this->transport_->swap_bytes());

  MulticastPeer remote_peer;  // sent as local_peer
  MulticastPeer local_peer;   // sent as remote peer

  serializer >> remote_peer;
  serializer >> local_peer;

  // Ignore message if not destined for us:
  if (local_peer != this->local_peer_) return;

  this->syn_watchdog_.cancel();

  // 2-way handshake is complete; we have verified that the remote
  // peer is indeed sending/receiving data reliably. Adjust the
  // acked flag and force the TransportImpl to re-evaluate any
  // pending associations it has queued:
  this->acked_ = true;
  this->transport_->check_fully_association();
}

void
ReliableMulticast::send_synack(MulticastPeer remote_peer)
{
  size_t len = sizeof (this->local_peer_)
             + sizeof (remote_peer);

  ACE_Message_Block* data;
  ACE_NEW_NORETURN(data, ACE_Message_Block(len));
  if (data == 0) {
    ACE_ERROR((LM_ERROR,
               ACE_TEXT("(%P|%t) ERROR: ")
               ACE_TEXT("ReliableMulticast::send_synack: ")
               ACE_TEXT("failed to allocate message!\n")));
    return;
  }

  TAO::DCPS::Serializer serializer(
    data, this->transport_->swap_bytes());

  serializer << this->local_peer_;
  serializer << remote_peer;

  ACE_Message_Block* message =
    create_control(MULTICAST_SYNACK, data);

  int error;
  if ((error = send_control(message)) != SEND_CONTROL_OK) {
    ACE_ERROR((LM_ERROR,
               ACE_TEXT("(%P|%t) ERROR: ")
               ACE_TEXT("ReliableMulticast::send_synack: ")
               ACE_TEXT("send_control failed: %d!\n"),
               error));
    return;
  }
}

void
ReliableMulticast::nak_received(ACE_Message_Block* message)
{
  // TODO implement
}

void
ReliableMulticast::send_nak(MulticastPeer remote_peer,
                            MulticastSequence low,
                            MulticastSequence high)
{
  size_t len = sizeof (this->local_peer_)
             + sizeof (remote_peer)
             + sizeof (low)
             + sizeof (high);

  ACE_Message_Block* data;
  ACE_NEW_NORETURN(data, ACE_Message_Block(len));
  if (data == 0) {
    ACE_ERROR((LM_ERROR,
               ACE_TEXT("(%P|%t) ERROR: ")
               ACE_TEXT("ReliableMulticast::send_nak: ")
               ACE_TEXT("failed to allocate message!\n")));
    return;
  }

  TAO::DCPS::Serializer serializer(
    data, this->transport_->swap_bytes());

  serializer << this->local_peer_;
  serializer << remote_peer;
  serializer << low;
  serializer << high;

  ACE_Message_Block* message =
    create_control(MULTICAST_NAK, data);

  int error;
  if ((error = send_control(message)) != SEND_CONTROL_OK) {
    ACE_ERROR((LM_ERROR,
               ACE_TEXT("(%P|%t) ERROR: ")
               ACE_TEXT("ReliableMulticast::send_nak: ")
               ACE_TEXT("send_control failed: %d!\n"),
               error));
    return;
  }
}

void
ReliableMulticast::nakack_received(ACE_Message_Block* message)
{
  // TODO implement
}

void
ReliableMulticast::send_nakack(MulticastPeer remote_peer,
                               MulticastSequence low,
                               MulticastSequence high)
{
  // TODO implement
}

bool
ReliableMulticast::join_i(const ACE_INET_Addr& /*group_address*/, bool active)
{
  ACE_Reactor* reactor = get_reactor();
  if (reactor == 0) {
    ACE_ERROR_RETURN((LM_ERROR,
                      ACE_TEXT("(%P|%t) ERROR: ")
                      ACE_TEXT("ReliableMulticast::join_i: ")
                      ACE_TEXT("NULL reactor reference!\n")),
                     false);
  }

  // A watchdog timer is scheduled to periodically check for gaps in
  // received data. If a gap is discovered, MULTICAST_NAK control
  // messages will be broadcasted to initiate resends from
  // responsible remote peers.
  if (!this->nak_watchdog_.schedule(reactor)) {
    ACE_ERROR_RETURN((LM_ERROR,
                      ACE_TEXT("(%P|%t) ERROR: ")
                      ACE_TEXT("ReliableMulticast::join_i: ")
                      ACE_TEXT("failed to schedule NAK watchdog timer!\n")),
                     false);
  }

  if (active) {
    // Active peers schedule a watchdog timer to initiate a 2-way
    // handshake to verify that passive endpoints can send/receive
    // data reliably. This process must be executed using the
    // transport reactor thread to prevent blocking.
    if (!this->syn_watchdog_.schedule(reactor)) {
      this->nak_watchdog_.cancel();
      ACE_ERROR_RETURN((LM_ERROR,
                        ACE_TEXT("(%P|%t) ERROR: ")
                        ACE_TEXT("ReliableMulticast::join_i: ")
                        ACE_TEXT("failed to schedule SYN watchdog timer!\n")),
                       false);
    }
  }

  return true;
}

void
ReliableMulticast::leave_i()
{
  this->syn_watchdog_.cancel();
  this->nak_watchdog_.cancel();
}

} // namespace DCPS
} // namespace OpenDDS
