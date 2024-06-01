/*
  ==============================================================================

   This file is part of the JUCE library.
   Copyright (c) 2022 - Raw Material Software Limited

   JUCE is an open source library subject to commercial or open-source
   licensing.

   The code included in this file is provided under the terms of the ISC license
   http://www.isc.org/downloads/software-support-policy/isc-license. Permission
   To use, copy, modify, and/or distribute this software for any purpose with or
   without fee is hereby granted provided that the above copyright notice and
   this permission notice appear in all copies.

   JUCE IS PROVIDED "AS IS" WITHOUT ANY WARRANTY, AND ALL WARRANTIES, WHETHER
   EXPRESSED OR IMPLIED, INCLUDING MERCHANTABILITY AND FITNESS FOR PURPOSE, ARE
   DISCLAIMED.

  ==============================================================================
*/

#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>

namespace juce
{

struct AndroidMessageQueue {
    JUCE_DECLARE_SINGLETON_SINGLETHREADED (AndroidMessageQueue, true)

    AndroidMessageQueue() : quitFlag(false) {
        messageThread = std::thread(&AndroidMessageQueue::processMessages, this);
    }

    ~AndroidMessageQueue() {
        JUCE_ASSERT_MESSAGE_THREAD
        quitFlag = true;
        cv.notify_one();
        messageThread.join();
    }

    bool post(MessageManager::MessageBase::Ptr&& message) {
        if (! quitFlag) {
            queue.add(message);
            std::lock_guard<std::mutex> lock(cv_m);
            cv.notify_one();
            return true;
        }
        return false;
    }

private:
    void processMessages() {
        while (! quitFlag) {
            std::unique_lock<std::mutex> lock(cv_m);
            cv.wait(lock, [this] { return !queue.isEmpty() || quitFlag; });

            while (! queue.isEmpty()) {
                auto message = queue.removeAndReturn(0);
                if (message != nullptr)
                    message->messageCallback();
            }
        }
    }

    ReferenceCountedArray<MessageManager::MessageBase, CriticalSection> queue;
    std::condition_variable cv;
    std::mutex cv_m;
    std::thread messageThread;
    std::atomic<bool> quitFlag;
};

JUCE_IMPLEMENT_SINGLETON (AndroidMessageQueue)

//==============================================================================
void MessageManager::doPlatformSpecificInitialisation() { 
    juce::Logger::writeToLog ("MessageManager::doPlatformSpecificInitialisation");
    AndroidMessageQueue::getInstance();
}
void MessageManager::doPlatformSpecificShutdown()       { AndroidMessageQueue::deleteInstance(); }

bool MessageManager::postMessageToSystemQueue (MessageManager::MessageBase* const message)
{
    return AndroidMessageQueue::getInstance()->post (message);
}

//==============================================================================
void MessageManager::broadcastMessage (const String&)
{
}

void MessageManager::runDispatchLoop()
{
}

void MessageManager::stopDispatchLoop()
{
    struct QuitCallback final : public CallbackMessage
    {
        QuitCallback() {}

        void messageCallback() override
        {
            auto* env = getEnv();
            LocalRef<jobject> activity (getCurrentActivity());

            if (activity != nullptr)
            {
                jmethodID quitMethod = env->GetMethodID (AndroidActivity, "finishAndRemoveTask", "()V");

                if (quitMethod != nullptr)
                {
                    env->CallVoidMethod (activity.get(), quitMethod);
                    return;
                }

                quitMethod = env->GetMethodID (AndroidActivity, "finish", "()V");
                jassert (quitMethod != nullptr);
                env->CallVoidMethod (activity.get(), quitMethod);
            }
            else
            {
                jassertfalse;
            }
        }
    };

    (new QuitCallback())->post();
    quitMessagePosted = true;
}

//==============================================================================
class JuceAppLifecycle final : public ActivityLifecycleCallbacks
{
public:
    JuceAppLifecycle (juce::JUCEApplicationBase* (*initSymbolAddr)())
        : createApplicationSymbol (initSymbolAddr)
    {
        LocalRef<jobject> appContext (getAppContext());

        if (appContext != nullptr)
        {
            auto* env = getEnv();

            myself = GlobalRef (CreateJavaInterface (this, "android/app/Application$ActivityLifecycleCallbacks"));
            env->CallVoidMethod (appContext.get(), AndroidApplication.registerActivityLifecycleCallbacks, myself.get());
        }
    }

    ~JuceAppLifecycle() override
    {
        LocalRef<jobject> appContext (getAppContext());

        if (appContext != nullptr && myself != nullptr)
        {
            auto* env = getEnv();

            clear();
            env->CallVoidMethod (appContext.get(), AndroidApplication.unregisterActivityLifecycleCallbacks, myself.get());
            myself.clear();
        }
    }

    void onActivityCreated (jobject, jobject) override
    {
        checkCreated();
    }

    void onActivityDestroyed (jobject activity) override
    {
        auto* env = getEnv();

        // if the main activity is being destroyed, only then tear-down JUCE
        if (env->IsSameObject (getMainActivity().get(), activity) != 0)
        {
            JUCEApplicationBase::appWillTerminateByForce();
            JNIClassBase::releaseAllClasses (env);

            jclass systemClass = (jclass) env->FindClass ("java/lang/System");
            jmethodID exitMethod = env->GetStaticMethodID (systemClass, "exit", "(I)V");
            env->CallStaticVoidMethod (systemClass, exitMethod, 0);
        }
    }

    void onActivityStarted (jobject) override
    {
        checkCreated();
    }

    void onActivityPaused (jobject) override
    {
        if (auto* app = JUCEApplicationBase::getInstance())
            app->suspended();
    }

    void onActivityResumed (jobject) override
    {
        checkInitialised();

        if (auto* app = JUCEApplicationBase::getInstance())
            app->resumed();
    }

    static JuceAppLifecycle& getInstance (juce::JUCEApplicationBase* (*initSymbolAddr)())
    {
        static JuceAppLifecycle juceAppLifecycle (initSymbolAddr);
        return juceAppLifecycle;
    }

private:
    void checkCreated()
    {
        if (JUCEApplicationBase::getInstance() == nullptr)
        {
            DBG (SystemStats::getJUCEVersion());

            JUCEApplicationBase::createInstance = createApplicationSymbol;

            initialiseJuce_GUI();

            if (! JUCEApplicationBase::createInstance())
                jassertfalse; // you must supply an application object for an android app!

            jassert (MessageManager::getInstance()->isThisTheMessageThread());
        }
    }

    void checkInitialised()
    {
        checkCreated();

        if (! hasBeenInitialised)
        {
            if (auto* app = JUCEApplicationBase::getInstance())
            {
                hasBeenInitialised = app->initialiseApp();

                if (! hasBeenInitialised)
                    exit (app->shutdownApp());
            }
        }
    }

    GlobalRef myself;
    juce::JUCEApplicationBase* (*createApplicationSymbol)();
    bool hasBeenInitialised = false;
};

//==============================================================================
File juce_getExecutableFile();

void juce_juceEventsAndroidStartApp();
void juce_juceEventsAndroidStartApp()
{
    auto dllPath = juce_getExecutableFile().getFullPathName();
    auto addr = reinterpret_cast<juce::JUCEApplicationBase*(*)()> (DynamicLibrary (dllPath)
                                                                    .getFunction ("juce_CreateApplication"));

    juce::Logger::writeToLog ("Starting JUCE app on Android");

    if (addr != nullptr)
    {
        juce::Logger::writeToLog ("juce_CreateApplication could create a juce app instance");
        JuceAppLifecycle::getInstance (addr);
    }
    else
    {
        juce::Logger::writeToLog ("juce_CreateApplication could not create a juce app instance, or not found in the library");
    }
}

} // namespace juce
