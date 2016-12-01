// Licensed under the Apache License, Version 2.0.
// Author: Jin Qing (http://blog.csdn.net/jq0123)

#include <grpc_cb/impl/client/client_async_reader_writer_impl.h>

#include <grpc_cb/channel.h>  // for MakeSharedCall()
#include <grpc_cb/impl/client/client_init_md_cqtag.h>  // ClientInitMdCqTag
#include <grpc_cb/impl/client/client_send_close_cqtag.h>  // for ClientSendCloseCqTag

#include "client_async_reader_helper.h"  // for ClientAsyncReaderHelper
#include "client_async_writer_helper.h"  // for ClientAsyncWriterHelper

namespace grpc_cb {

using Sptr = std::shared_ptr<ClientAsyncReaderWriterImpl>;

// Todo: BlockingGetInitMd();

ClientAsyncReaderWriterImpl::ClientAsyncReaderWriterImpl(
    const ChannelSptr& channel, const std::string& method,
    const CompletionQueueSptr& cq_sptr)
    : cq_sptr_(cq_sptr), 
    call_sptr_(channel->MakeSharedCall(method, *cq_sptr)) {
  assert(cq_sptr);
  ClientInitMdCqTag* tag = new ClientInitMdCqTag(call_sptr_);
  if (tag->Start()) return;
  delete tag;
  status_.SetInternalError("Failed to init stream.");
}

ClientAsyncReaderWriterImpl::~ClientAsyncReaderWriterImpl() {
  CloseWritingNow();
}

bool ClientAsyncReaderWriterImpl::Write(const MessageSptr& msg_sptr) {
  assert(call_sptr_);
  Guard g(mtx_);

  if (!status_.ok())
    return false;

  if (!writer_uptr_) {
    Sptr sptr = shared_from_this();
    writer_uptr_.reset(new ClientAsyncWriterHelper(call_sptr_, status_,
        [sptr]() {
          sptr->WriteNext();
        }));
  }

  writer_uptr_->Write(msg_sptr);
  return true;
}

void ClientAsyncReaderWriterImpl::CloseWriting() {
  Guard g(mtx_);
  // Set to send close when all messages are written.
  can_close_writing_ = true;
}

// private
void ClientAsyncReaderWriterImpl::CloseWritingNow() {
  if (!status_.ok()) return;
  if (writer_uptr_->IsWritingClosed()) return;
  writer_uptr_->SetWritingClosed();

  ClientSendCloseCqTag* tag = new ClientSendCloseCqTag(call_sptr_);
  if (tag->Start()) return;

  delete tag;
  status_.SetInternalError("Failed to set stream writes done.");
}

// Todo: same as ClientReader?

void ClientAsyncReaderWriterImpl::SetReadHandler(
    const ReadHandlerSptr& handler_sptr) {
  Guard g(mtx_);

  read_handler_sptr_ = handler_sptr;
  if (is_reading_) return;
  is_reading_ = true;

  auto sptr = shared_from_this();
  if (!reader_uptr_)
    reader_uptr_.reset(new ClientAsyncReaderHelper);
  // XXX reader_uptr_->AsyncReadNext();
  // XXX ClientAsyncReaderHelper::AsyncReadNext(on_read, on_end)
  // XXX ClientAsyncReaderHelper::AsyncReadNext(data_sptr_);  // XXX
}

void ClientAsyncReaderWriterImpl::WriteNext() {
  Guard g(mtx_);

  // Called from the write completion callback.
  assert(writer_uptr_);
  assert(writer_uptr_->IsWriting());
  InternalNext();
}

// Send messages one by one, and finally close.
void ClientAsyncReaderWriterImpl::InternalNext() {
  assert(writer_uptr_);
  if (writer_uptr_->WriteNext())
    return;

  if (can_close_writing_)
    CloseWritingNow();
}

}  // namespace grpc_cb