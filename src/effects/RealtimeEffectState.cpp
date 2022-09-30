/**********************************************************************

  Audacity: A Digital Audio Editor

  @file RealtimeEffectState.cpp

  Paul Licameli split from RealtimeEffectManager.cpp

 *********************************************************************/

#include "RealtimeEffectState.h"

#include "AudioIOBase.h"
#include "EffectInterface.h"
#include "MessageBuffer.h"
#include "PluginManager.h"
#include "SampleCount.h"

#include <chrono>
#include <thread>

//! Mediator of two-way inter-thread communication of changes of settings
class RealtimeEffectState::AccessState : public NonInterferingBase {
public:
   //! @pre `state.mMainSettings.settings.has_value()`
   //! @invariant `mLastSettings.settings.has_value()`
   AccessState(const EffectSettingsManager &effect, RealtimeEffectState &state)
      : mEffect{ effect }
      , mState{ state }
   {
      Initialize(state.mMainSettings, state.mMovedOutputs.get());
   }

   //! @pre `settings.settings.has_value()`
   //! @post `mLastSettings.settings.has_value()`
   void Initialize(SettingsAndCounter &settings,
      const EffectOutputs *pOutputs)
   {
      assert(settings.settings.has_value());

      // Clean initial state of the counter
      settings.counter = 0;
      mLastSettings = settings;
      // Initialize each message buffer with two copies of settings
      mChannelToMain.Write(ToMainSlot{ { 0,
         pOutputs ? pOutputs->Clone() : nullptr } });
      mChannelToMain.Write(ToMainSlot{ { 0,
         pOutputs ? pOutputs->Clone() : nullptr } });
      mChannelFromMain.Write(FromMainSlot{ settings });
      mChannelFromMain.Write(FromMainSlot{ settings });

      assert(mLastSettings.settings.has_value());
   }

   void MainRead() {
      mChannelToMain.Read<ToMainSlot::Reader>(mState.mMovedOutputs.get(),
         mCounter);
   }
   void MainWrite(SettingsAndCounter &&settings) {
      // Main thread may simply swap new content into place
      mChannelFromMain.Write(std::move(settings));
   }

   struct CounterAndOutputs{
      Response::Counter counter;
      EffectOutputs *pOutputs{};
   };
   void WorkerRead() {
      // Worker thread avoids memory allocation.  It copies the contents of any
      mChannelFromMain.Read<FromMainSlot::Reader>(
         mEffect, mState.mWorkerSettings);
   }
   void WorkerWrite() {
      // Worker thread avoids memory allocation.
      mChannelToMain.Write(CounterAndOutputs{
         mState.mWorkerSettings.counter, mState.mOutputs.get() });
   }

   struct ToMainSlot {
      // For initialization of the channel
      ToMainSlot() = default;
      explicit ToMainSlot(Response response)
         : mResponse{ std::move(response) }
      {}
      ToMainSlot &operator=(ToMainSlot &&) = default;

      // Worker thread writes the slot
      ToMainSlot& operator=(CounterAndOutputs &&arg) {
         // This happens during MessageBuffer's busying of the slot
         mResponse.counter = arg.counter;
         if (mResponse.pOutputs && arg.pOutputs)
            mResponse.pOutputs->Assign(std::move(*arg.pOutputs));
         return *this;
      }

      struct Reader { Reader(ToMainSlot &&slot, EffectOutputs *pOutputs,
         Response::Counter &counter
      ) {
         // Main thread is not under the performance constraints of the
         // worker, but Assign is still used so that
         // members of underlying vectors or other containers do not
         // relocate
         if (pOutputs && slot.mResponse.pOutputs)
            pOutputs->Assign(std::move(*slot.mResponse.pOutputs));
         counter = slot.mResponse.counter;
      } };

      Response mResponse;
   };

   struct FromMainSlot {
      // For initialization of the channel
      FromMainSlot() = default;
      explicit FromMainSlot(const SettingsAndCounter &settings)
         // Copy std::any
         : mSettings{ settings }
      {}
      FromMainSlot &operator=(FromMainSlot &&) = default;

      // Main thread writes the slot
      FromMainSlot& operator=(SettingsAndCounter &&settings) {
         mSettings.swap(settings);
         return *this;
      }

      // Worker thread reads the slot
      struct Reader { Reader(FromMainSlot &&slot,
         const EffectSettingsManager &effect, SettingsAndCounter &settings) {
         if(slot.mSettings.counter == settings.counter)
            return;//copy once

         settings.counter = slot.mSettings.counter;
            // This happens during MessageBuffer's busying of the slot
            effect.CopySettingsContents(
               slot.mSettings.settings, settings.settings);
            settings.settings.extra = slot.mSettings.settings.extra;
      } };

      SettingsAndCounter mSettings;
   };

   const EffectSettingsManager &mEffect;
   RealtimeEffectState &mState;

   MessageBuffer<FromMainSlot> mChannelFromMain;
   Response::Counter mCounter;
   SettingsAndCounter mLastSettings;

   MessageBuffer<ToMainSlot> mChannelToMain;
};

//! Main thread's interface to inter-thread communication of changes of settings
struct RealtimeEffectState::Access final : EffectSettingsAccess {
   Access() = default;
   explicit Access(RealtimeEffectState &state)
      : mwState{ state.weak_from_this() }
   {
   }
   ~Access() override = default;
   // Try once to detect that the worker thread has echoed the last write
   // @pre state.mState.mInitialized
   static bool FlushAttempt(AccessState &state) {
      assert(state.mState.mInitialized);
      auto &lastSettings = state.mLastSettings;
      // Assigns to mCounter
      state.MainRead();
      // If true, then first time test, or echo is completed
      return state.mCounter == lastSettings.counter;
   }
   const EffectSettings &Get() override {
      if (auto pState = mwState.lock()) {
         if (auto pAccessState = pState->GetAccessState()) {
            if (pAccessState->mState.mInitialized)
               FlushAttempt(*pAccessState); // try once, ignore success
            else {
               // Not yet waiting on the other thread's progress
               // Not necessarily values yet in the state's Settings objects
            }
            return pAccessState->mLastSettings.settings;
         }
      }
      // Non-modal dialog may have outlived the RealtimeEffectState
      static EffectSettings empty;
      return empty;
   }
   void Set(EffectSettings &&settings) override {
      if (!settings.has_value())
         // Protect the invariant!
         return;
      if (auto pState = mwState.lock())
         if (auto pAccessState = pState->GetAccessState()) {
            auto &lastSettings = pAccessState->mLastSettings;
            // move to remember values here
            lastSettings.settings = std::move(settings);
            ++lastSettings.counter;
            // move a copy to there
            pAccessState->MainWrite(SettingsAndCounter{ lastSettings });
         }
   }
   void Flush() override {
      if (auto pState = mwState.lock()) {
         if (auto pAccessState = pState->GetAccessState()) {
            bool bResult{};
            if (pAccessState->mState.mInitialized) {
               while (!(bResult = FlushAttempt(*pAccessState))) {
                  // Wait for progress of audio thread
                  using namespace std::chrono;
                  std::this_thread::sleep_for(50ms);
               }
            }

            // Update what GetSettings() will return, during play and before
            // Finalize(), but after it is confirmed that any worker thread has
            // seen the values given to the last Set().  These values will also
            // be returned by Get().
            pState->mMainSettings.Set(pAccessState->mLastSettings);
         }
      }
   }
   bool IsSameAs(const EffectSettingsAccess &other) const override {
      if (auto pOther = dynamic_cast<const Access*>(&other)) {
         auto &mine = mwState;
         auto &theirs = pOther->mwState;
         auto less = std::owner_less{};
         return !(less(mine, theirs) || less(theirs, mine));
      }
      return false;
   }
   //! Store no state here but this weak pointer, so `IsSameAs` isn't lying
   std::weak_ptr<RealtimeEffectState> mwState;
};

RealtimeEffectState::RealtimeEffectState(const PluginID& id)
{
   SetID(id);
   BuildAll();
}

RealtimeEffectState::~RealtimeEffectState()
{

}

void RealtimeEffectState::SetID(const PluginID & id)
{
   bool empty = id.empty();
   if (mID.empty() && !empty) {
      mID = id;
      GetEffect();
   }
   else
      // Set mID to non-empty at most once
      assert(empty);
}

const PluginID& RealtimeEffectState::GetID() const noexcept
{
   return mID;
}

const EffectInstanceFactory *RealtimeEffectState::GetEffect()
{
   if (!mPlugin) {
      mPlugin = EffectFactory::Call(mID);
      if (mPlugin) {
         // Also make EffectSettings, but preserve activation
         auto wasActive = mMainSettings.settings.extra.GetActive();
         mMainSettings.counter = 0;
         mMainSettings.settings = mPlugin->MakeSettings();
         mMainSettings.settings.extra.SetActive(wasActive);
         mOutputs = mPlugin->MakeOutputs();
         mMovedOutputs = mPlugin->MakeOutputs();
      }
   }
   return mPlugin;
}

std::shared_ptr<EffectInstance>
RealtimeEffectState::EnsureInstance(double sampleRate)
{
   if (!mPlugin)
      return {};

   auto pInstance = mwInstance.lock();
   if (!mInitialized) {
      //! copying settings in the main thread while worker isn't yet running
      mWorkerSettings = mMainSettings;
      mLastActive = IsActive();

      //! If there was already an instance, recycle it; else make one here
      if (!pInstance)
         mwInstance = pInstance = mPlugin->MakeInstance();
      if (!pInstance)
         return {};

      // PRL: conserving pre-3.2.0 behavior, but I don't know why this arbitrary
      // number was important
      pInstance->SetBlockSize(512);

      if (!pInstance->RealtimeInitialize(mMainSettings.settings, sampleRate))
         return {};
      mInitialized = true;
      return pInstance;
   }
   return pInstance;
}

std::shared_ptr<EffectInstance> RealtimeEffectState::GetInstance()
{
   //! If there was already an instance, recycle it; else make one here
   auto pInstance = mwInstance.lock();
   if (!pInstance && mPlugin)
      mwInstance = pInstance = mPlugin->MakeInstance();
   return pInstance;
}

std::shared_ptr<EffectInstance>
RealtimeEffectState::Initialize(double sampleRate)
{
   if (!mPlugin)
      return {};

   mCurrentProcessor = 0;
   mGroups.clear();
   mLatency = {};
   return EnsureInstance(sampleRate);
}

namespace {
// The caller passes the number of channels to process and specifies
// the number of input and output buffers.  There will always be the
// same number of output buffers as there are input buffers.
//
// Effects require a certain number of input and output buffers.
// The number of channels we're currently processing may mismatch
// the effect's requirements.  Allocate some inputs repeatedly to a processor
// that needs more, or allocate multiple processors if they accept too few.
// Continue until the output buffers are all allocated.
template<typename F>
void AllocateChannelsToProcessors(
   unsigned chans, const unsigned numAudioIn, const unsigned numAudioOut,
   const F &f)
{
   unsigned indx = 0;
   for (unsigned ondx = 0; ondx < chans; ondx += numAudioOut) {
      // Pass the function indices into the arrays of buffers
      if (!f(indx, ondx))
         return;
      indx += numAudioIn;
      indx %= chans;
   }
}
}

//! Set up processors to be visited repeatedly in Process.
/*! The iteration over channels in AddTrack and Process must be the same */
std::shared_ptr<EffectInstance>
RealtimeEffectState::AddTrack(Track &track, unsigned chans, float sampleRate)
{
   auto pInstance = EnsureInstance(sampleRate);
   if (!pInstance)
      return {};
   if (!mPlugin)
      return {};
   auto first = mCurrentProcessor;
   const auto numAudioIn = pInstance->GetAudioInCount();
   const auto numAudioOut = pInstance->GetAudioOutCount();
   AllocateChannelsToProcessors(chans, numAudioIn, numAudioOut,
   [&](unsigned, unsigned){
      // Add a NEW processor
      if (pInstance->RealtimeAddProcessor(
         mWorkerSettings.settings, mOutputs.get(), numAudioIn, sampleRate)
      ) {
         mCurrentProcessor++;
         return true;
      }
      else
         return false;
   });
   if (mCurrentProcessor > first) {
      // Remember the sampleRate of the track, so latency can be computed later
      mGroups[&track] = { first, sampleRate };
      return pInstance;
   }
   return {};
}

bool RealtimeEffectState::ProcessStart(bool running)
{
   // Get state changes from the main thread
   // Note that it is only here that the answer of IsActive() may be changed,
   // and it is important that for each state the answer is unchanging in one
   // processing scope.
   if (auto pAccessState = TestAccessState())
      pAccessState->WorkerRead();

   // Detect transitions of activity state
   auto pInstance = mwInstance.lock();
   bool active = IsActive() && running;
   if (active != mLastActive) {
      if (pInstance) {
         bool success = active
            ? pInstance->RealtimeResume()
            : pInstance->RealtimeSuspend();
         if (!success)
            return false;
      }
      mLastActive = active;
   }

   if (!pInstance || !active)
      return false;

   // Assuming we are in a processing scope, use the worker settings
   return pInstance->RealtimeProcessStart(mWorkerSettings.settings);
}

#define stackAllocate(T, count) static_cast<T*>(alloca(count * sizeof(T)))

//! Visit the effect processors that were added in AddTrack
/*! The iteration over channels in AddTrack and Process must be the same */
size_t RealtimeEffectState::Process(Track &track, unsigned chans,
   const float *const *inbuf, float *const *outbuf, size_t numSamples)
{
   auto pInstance = mwInstance.lock();
   if (!mPlugin || !pInstance || !mLastActive) {
      // Process trivially
      for (size_t ii = 0; ii < chans; ++ii)
         memcpy(outbuf[ii], inbuf[ii], numSamples * sizeof(float));
      return 0;
   }
   const auto numAudioIn = pInstance->GetAudioInCount();
   const auto numAudioOut = pInstance->GetAudioOutCount();
   const auto clientIn = stackAllocate(const float *, numAudioIn);
   const auto clientOut = stackAllocate(float *, numAudioOut);
   size_t len = 0;
   const auto &pair = mGroups[&track];
   auto processor = pair.first;
   // Outer loop over processors
   AllocateChannelsToProcessors(chans, numAudioIn, numAudioOut,
   [&](unsigned indx, unsigned ondx){
      // Point at the correct input buffers
      unsigned copied = std::min(chans - indx, numAudioIn);
      std::copy(inbuf + indx, inbuf + indx + copied, clientIn);
      // If there are too few input channels for what the processor requires,
      // re-use input channels from the beginning
      while (auto need = numAudioIn - copied) {
         auto moreCopied = std::min(chans, need);
         std::copy(inbuf, inbuf + moreCopied, clientIn + copied);
         copied += moreCopied;
      }

      // Point at the correct output buffers
      copied = std::min(chans - ondx, numAudioOut);
      std::copy(outbuf + ondx, outbuf + ondx + copied, clientOut);
      if (copied < numAudioOut) {
         // Make determinate pointers
         std::fill(clientOut + copied, clientOut + numAudioOut, nullptr);
      }

      // Inner loop over blocks
      const auto blockSize = pInstance->GetBlockSize();
      for (size_t block = 0; block < numSamples; block += blockSize) {
         auto cnt = std::min(numSamples - block, blockSize);
         // Assuming we are in a processing scope, use the worker settings
         auto processed = pInstance->RealtimeProcess(processor,
            mWorkerSettings.settings, clientIn, clientOut, cnt);
         if (!mLatency)
            // Find latency once only per initialization scope,
            // after processing one block
            mLatency.emplace(
               pInstance->GetLatency(mWorkerSettings.settings, pair.second));
         for (size_t i = 0 ; i < numAudioIn; i++)
            if (clientIn[i])
               clientIn[i] += cnt;
         for (size_t i = 0 ; i < numAudioOut; i++)
            if (clientOut[i])
               clientOut[i] += cnt;
         if (ondx == 0) {
            // For the first processor only
            len += processed;
            auto discard = limitSampleBufferSize(len, *mLatency);
            len -= discard;
            *mLatency -= discard;
         }
      }
      ++processor;
      return true;
   });
   // Report the number discardable during the processing scope
   // We are assuming len as calculated above is the same in case of multiple
   // processors
   return numSamples - len;
}

bool RealtimeEffectState::ProcessEnd()
{
   auto pInstance = mwInstance.lock();
   bool result = pInstance && IsActive() && mLastActive &&
      // Assuming we are in a processing scope, use the worker settings
      pInstance->RealtimeProcessEnd(mWorkerSettings.settings);

   if (auto pAccessState = TestAccessState())
      // Always done, regardless of activity
      // Some dialogs require communication back from the processor so that
      // they can update their appearance in idle time, and some plug-in
      // libraries (like lv2) require the host program to mediate the
      // communication
      pAccessState->WorkerWrite();

   return result;
}

bool RealtimeEffectState::IsEnabled() const noexcept
{
   return mMainSettings.settings.extra.GetActive();
}

bool RealtimeEffectState::IsActive() const noexcept
{
   return mWorkerSettings.settings.extra.GetActive();
}

void RealtimeEffectState::SetActive(bool active)
{
   auto access = GetAccess();
   access->ModifySettings([&](EffectSettings &settings) {
      settings.extra.SetActive(active);
   });
   access->Flush();

   Publish(active
      ? RealtimeEffectStateChange::EffectOn
      : RealtimeEffectStateChange::EffectOff);
}

bool RealtimeEffectState::Finalize() noexcept
{
   // This is the main thread cleaning up a state not now used in processing
   mMainSettings = mWorkerSettings;

   mGroups.clear();
   mCurrentProcessor = 0;

   auto pInstance = mwInstance.lock();
   if (!pInstance)
      return false;

   auto result = pInstance->RealtimeFinalize(mMainSettings.settings);
   mLatency = {};
   mInitialized = false;
   return result;
}

const std::string &RealtimeEffectState::XMLTag()
{
   static const std::string result{"effect"};
   return result;
}

static const auto idAttribute = "id";
static const auto versionAttribute = "version";
static const auto parametersAttribute = "parameters";
static const auto parameterAttribute = "parameter";
static const auto nameAttribute = "name";
static const auto valueAttribute = "value";
static constexpr auto activeAttribute = "active";

bool RealtimeEffectState::HandleXMLTag(
   const std::string_view &tag, const AttributesList &attrs)
{
   if (tag == XMLTag()) {
      mParameters.clear();
      mPlugin = nullptr;
      mID.clear();
      for (auto &[attr, value] : attrs) {
         if (attr == idAttribute) {
            SetID(value.ToWString());
            if (!mPlugin) {
               // TODO - complain!!!!
            }
         }
         else if (attr == versionAttribute) {
         }
         else if (attr == activeAttribute)
            // Updating the EffectSettingsExtra although we haven't yet built
            // the settings
            mMainSettings.settings.extra.SetActive(value.Get<bool>());
      }
      return true;
   }
   else if (tag == parametersAttribute)
      return true;
   else if (tag == parameterAttribute) {
      wxString n;
      wxString v;
      for (auto &[attr, value] : attrs) {
         if (attr == nameAttribute)
            n = value.ToWString();
         else if (attr == valueAttribute)
            v = value.ToWString();
      }
      mParameters += wxString::Format(wxT("\"%s=%s\" "), n, v);
      return true;
   }
   else
      return false;
}

void RealtimeEffectState::HandleXMLEndTag(const std::string_view &tag)
{
   if (tag == XMLTag()) {
      if (mPlugin && !mParameters.empty()) {
         CommandParameters parms(mParameters);
         mPlugin->LoadSettings(parms, mMainSettings.settings);
      }
      mParameters.clear();
   }
}

XMLTagHandler *RealtimeEffectState::HandleXMLChild(const std::string_view &tag)
{
   // Tag may be for the state, or the list of parameters, or for one parameter.
   // See the writing method below.  All are handled by this
   return this;
}

void RealtimeEffectState::WriteXML(XMLWriter &xmlFile)
{
   if (!mPlugin)
      return;

   xmlFile.StartTag(XMLTag());
   const auto active = mMainSettings.settings.extra.GetActive();
   xmlFile.WriteAttr(activeAttribute, active);
   xmlFile.WriteAttr(idAttribute, PluginManager::GetID(mPlugin));
   xmlFile.WriteAttr(versionAttribute, mPlugin->GetVersion());

   CommandParameters cmdParms;
   if (mPlugin->SaveSettings(mMainSettings.settings, cmdParms)) {
      xmlFile.StartTag(parametersAttribute);

      wxString entryName;
      long entryIndex;
      bool entryKeepGoing;

      entryKeepGoing = cmdParms.GetFirstEntry(entryName, entryIndex);
      while (entryKeepGoing) {
         wxString entryValue = cmdParms.Read(entryName, "");

         xmlFile.StartTag(parameterAttribute);
         xmlFile.WriteAttr(nameAttribute, entryName);
         xmlFile.WriteAttr(valueAttribute, entryValue);
         xmlFile.EndTag(parameterAttribute);

         entryKeepGoing = cmdParms.GetNextEntry(entryName, entryIndex);
      }

      xmlFile.EndTag(parametersAttribute);
   }

   xmlFile.EndTag(XMLTag());
}

std::shared_ptr<EffectSettingsAccess> RealtimeEffectState::GetAccess()
{
   if (!GetEffect() // Effect not found!
       || !mMainSettings.settings.has_value() // can't satisfy pre of ctor
   )
      // Return a dummy
      return std::make_shared<Access>();

   // Only the main thread assigns to the atomic pointer, here and
   // once only in the lifetime of the state
   if (!GetAccessState())
      mpAccessState.emplace(*mPlugin, *this);
   return std::make_shared<Access>(*this);
}
