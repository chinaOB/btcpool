/*
 The MIT License (MIT)

 Copyright (c) [2016] [BTC.COM]

 Permission is hereby granted, free of charge, to any person obtaining a copy
 of this software and associated documentation files (the "Software"), to deal
 in the Software without restriction, including without limitation the rights
 to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 copies of the Software, and to permit persons to whom the Software is
 furnished to do so, subject to the following conditions:

 The above copyright notice and this permission notice shall be included in
 all copies or substantial portions of the Software.

 THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 THE SOFTWARE.
*/

#include "StratumServerGrin.h"

#include "StratumSessionGrin.h"
#include "CommonGrin.h"

#include <boost/make_unique.hpp>
#include <algorithm>

unique_ptr<StratumSession> StratumServerGrin::createConnection(
  struct bufferevent *bev,
  struct sockaddr *saddr,
  uint32_t sessionID) {
  return boost::make_unique<StratumSessionGrin>(*this, bev, saddr, sessionID);
}

void StratumServerGrin::checkAndUpdateShare(
  size_t chainId,
  ShareGrin &share,
  shared_ptr<StratumJobEx> exjob,
  const vector<uint64_t > &proofs,
  const std::set<uint64_t> &jobDiffs,
  const string &workFullName) {
  auto sjob = dynamic_cast<StratumJobGrin *>(exjob->sjob_);

  DLOG(INFO) << "checking share nonce: " << std::hex << share.nonce()
             << ", pre_pow: " << sjob->prePow_
             << ", edge_bits: " << share.edgebits();

  if (exjob->isStale()) {
    share.set_status(StratumStatus::JOB_NOT_FOUND);
    return;
  }

  PreProofGrin preProof;
  auto bin = ParseHex(sjob->prePow_);
  memcpy(&preProof.prePow, bin.data(), sizeof(PreProofGrin));
  preProof.nonce = share.nonce();
  if (!VerifyPowGrin(preProof, share.edgebits(), proofs)) {
    share.set_status(StratumStatus::INVALID_SOLUTION);
    return;
  }

  uint64_t shareDiff = PowDifficultyGrin(share.height(), share.edgebits(), preProof.prePow.secondaryScaling.value(), proofs);
  DLOG(INFO) << "compare share difficulty: " << shareDiff << ", network difficulty: " << sjob->difficulty_;

  // print out high diff share
  if (shareDiff / sjob->difficulty_ >= 1024) {
    LOG(INFO) << "high diff share, share difficulty: " << shareDiff << ", network difficulty: " << sjob->difficulty_
              << ", worker: " << workFullName;
  }

  if (isSubmitInvalidBlock_ || shareDiff >= sjob->difficulty_) {
    LOG(INFO) << "solution found, share difficulty: " << shareDiff << ", network difficulty: " << sjob->difficulty_
              << ", worker: " << workFullName;

    share.set_status(StratumStatus::SOLVED);
    LOG(INFO) << "solved share: " << share.toString();
    return;
  }

  // higher difficulty is prior
  for (auto itr = jobDiffs.rbegin(); itr != jobDiffs.rend(); itr++) {
    uint64_t jobDiff = *itr;
    DLOG(INFO) << "compare share difficulty: " << shareDiff << ", job difficulty: " << jobDiff;

    if (isEnableSimulator_ || shareDiff >= jobDiff) {
      share.set_sharediff(jobDiff);
      share.set_status(StratumStatus::ACCEPT);
      return;
    }
  }

  share.set_status(StratumStatus::LOW_DIFFICULTY);
  return;
}

void StratumServerGrin::sendSolvedShare2Kafka(
  size_t chainId,
  const ShareGrin &share,
  shared_ptr<StratumJobEx> exjob,
  const vector<uint64_t> &proofs,
  const StratumWorker &worker) {
  string proofArray;
  if (!proofs.empty()) {
    proofArray = std::accumulate(
      std::next(proofs.begin()),
      proofs.end(),
      std::to_string(proofs.front()),
      [](string a, int b) { return std::move(a) + "," + std::to_string(b); });
  }

  auto sjob = dynamic_cast<StratumJobGrin *>(exjob->sjob_);
  string msg = Strings::Format(
    "{\"jobId\":%" PRIu64
    ",\"nodeJobId\":%" PRIu64
    ",\"height\":%" PRIu64
    ",\"edgeBits\":%" PRIu32
    ",\"nonce\":%" PRIu64
    ",\"proofs\":[%s]"
    ",\"userId\":%" PRId32
    ",\"workerId\":%" PRId64
    ",\"workerFullName\":\"%s\""
    "}",
    sjob->jobId_,
    sjob->nodeJobId_,
    sjob->height_,
    share.edgebits(),
    share.nonce(),
    proofArray.c_str(),
    worker.userId_,
    worker.workerHashId_,
    filterWorkerName(worker.fullName_).c_str());
  ServerBase::sendSolvedShare2Kafka(chainId, msg.c_str(), msg.length());
}

JobRepository* StratumServerGrin::createJobRepository(
  size_t chainId,
  const char *kafkaBrokers,
  const char *consumerTopic,
  const string &fileLastNotifyTime) {
  return new JobRepositoryGrin{chainId, this, kafkaBrokers, consumerTopic, fileLastNotifyTime};
}

JobRepositoryGrin::JobRepositoryGrin(
  size_t chainId,
  StratumServerGrin *server,
  const char *kafkaBrokers,
  const char *consumerTopic,
  const string &fileLastNotifyTime)
  : JobRepositoryBase{chainId, server, kafkaBrokers, consumerTopic, fileLastNotifyTime}
  , lastHeight_{0} {
}

StratumJob* JobRepositoryGrin::createStratumJob() {
  return new StratumJobGrin;
}

void JobRepositoryGrin::broadcastStratumJob(StratumJob *sjob) {
  auto sjobGrin = dynamic_cast<StratumJobGrin*>(sjob);

  LOG(INFO) << "broadcast stratum job " << std::hex << sjobGrin->jobId_;

  bool isClean = false;
  if (sjobGrin->height_ != lastHeight_) {
    isClean = true;
    lastHeight_ = sjobGrin->height_;

    LOG(INFO) << "received new height stratum job, height: " << sjobGrin->height_
              << ", prePow: " << sjobGrin->prePow_;
  }

  shared_ptr<StratumJobEx> exJob{createStratumJobEx(sjobGrin, isClean)};
  {
    ScopeLock sl(lock_);

    if (isClean) {
      // mark all jobs as stale, should do this before insert new job
      // stale shares will not be rejected, they will be marked as ACCEPT_STALE and have lower rewards.
      for (auto it : exJobs_) {
        it.second->markStale();
      }
    }

    // insert new job
    exJobs_[sjobGrin->jobId_] = exJob;
  }

  //send job
  sendMiningNotify(exJob);
}
 