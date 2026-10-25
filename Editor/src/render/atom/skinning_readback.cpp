#include "render/atom/skinning_readback.h"
#include <Atom/RPI.Public/Pass/AttachmentReadback.h>
#include <Atom/RPI.Public/Pass/PassFilter.h>
#include <Atom/RPI.Public/Pass/PassSystemInterface.h>
#include <AzCore/std/smart_ptr/make_shared.h>
#include <QEventLoop>
#include <QMutex>
#include <QMutexLocker>
#include <QPointer>
#include <QTimer>
#include <memory>

namespace jester::render {
QByteArray readSkinningOutput(AZ::RPI::Scene &scene,QString &error) {
    auto *passes=AZ::RPI::PassSystemInterface::Get();
    auto filter=AZ::RPI::PassFilter::CreateWithTemplateName(AZ::Name("SkinningPassTemplate"),&scene);
    auto *pass=passes->FindFirstPass(filter);
    if(!pass) {error="skinning_readback.pass_unavailable";return {};}
    struct Result {QMutex mutex;QByteArray bytes;bool finished=false;};
    auto result=std::make_shared<Result>();QEventLoop loop;QPointer<QEventLoop> guard(&loop);
    auto readback=AZStd::make_shared<AZ::RPI::AttachmentReadback>(AZ::Name("Jester skinning parity readback"));
    readback->SetCallback([result,guard](const AZ::RPI::AttachmentReadback::ReadbackResult &data) {
        {QMutexLocker lock(&result->mutex);result->finished=true;
            if(data.m_state==AZ::RPI::AttachmentReadback::ReadbackState::Success && data.m_dataBuffer)
                result->bytes=QByteArray(reinterpret_cast<const char*>(data.m_dataBuffer->data()),qsizetype(data.m_dataBuffer->size()));}
        if(guard)QMetaObject::invokeMethod(guard,&QEventLoop::quit,Qt::QueuedConnection);
    });
    if(!pass->ReadbackAttachment(readback,0,AZ::Name("SkinnedMeshOutputStream"))) {
        error="skinning_readback.request_failed";return {};
    }
    QTimer::singleShot(10000,&loop,&QEventLoop::quit);loop.exec(QEventLoop::ExcludeUserInputEvents);
    QMutexLocker lock(&result->mutex);
    if(!result->finished || result->bytes.isEmpty())error="skinning_readback.failed_or_timed_out";else error.clear();
    return result->bytes;
}
}
