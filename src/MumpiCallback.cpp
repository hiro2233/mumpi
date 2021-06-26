#include "MumpiCallback.hpp"

#ifdef __LIB_URUSSTUDIO__
static wimic_Callback wimic_callback = WIMIC::get_instance();
#endif // __LIB_URUSSTUDIO__

MumpiCallback::MumpiCallback(std::shared_ptr<RingBuffer<int16_t>> out_buf) :
        _out_buf(out_buf) {
#ifdef __LIB_URUSSTUDIO__
    wimic_callback.init(out_buf);
#endif
}

MumpiCallback::~MumpiCallback() {
#ifdef __LIB_URUSSTUDIO__
    wimic_callback.stop();
#endif // __LIB_URUSSTUDIO__
}

/**
 * Handles received serverSync messages (when connection established).
 *
 * @param welcome_text  welcome text
 * @param session       session
 * @param max_bandwidth max bandwidth
 * @param permissions   permissions
 */
void MumpiCallback::serverSync(std::string welcome_text,
                               int32_t session,
                               int32_t max_bandwidth,
                               int64_t permissions) {
    _logger.info("Joined server!");
    _logger.info(welcome_text);
}

/**
 * Handles received audio packets and pushes them to the circular buffer
 *
 * @param target         target
 * @param sessionId      session ifndef
 * @param sequenceNumber sequence number
 * @param pcm_data       raw PCM data (int16_t)
 * @param pcm_data_size  PCM data buf size
 */
void MumpiCallback::audio(int target,
                          int sessionId,
                          int sequenceNumber,
                          int16_t *pcm_data,
                          uint32_t pcm_data_size) {
    //printf("Received audio: pcm_data_size: %d target: %d sesseionID: %d sequence: %d\n", pcm_data_size, target, sessionId, sequenceNumber);
    //printf("\nMumpiCallback Received audio: pcm_data_size: %d\n", pcm_data_size);
    if(pcm_data != NULL) {
#ifdef __LIB_URUSSTUDIO__
        wimic_callback.update_audio(target, sessionId, sequenceNumber, pcm_data, pcm_data_size);
#else
        _out_buf->push(pcm_data, 0, pcm_data_size);
#endif // __LIB_URUSSTUDIO__
    }
}

/**
 * Handles received text messages
 * @param  actor      actor
 * @param  session    session
 * @param  channel_id channel id
 * @param  tree_id    tree id
 * @param  message    the message
 */
void MumpiCallback::textMessage(uint32_t actor,
                                std::vector<uint32_t> session,
                                std::vector<uint32_t> channel_id,
                                std::vector<uint32_t> tree_id,
                                std::string message) {
    _logger.info("Received text message: %s", message.c_str());
    printf("Received text message: %s actor: %lu session: %lu channel: %lu tree: %lu\n", message.c_str(), actor, session.size(), channel_id.size(), tree_id.size());
    //mumlib::BasicCallback::textMessage(actor, session, channel_id, tree_id, message);
    //mum->sendTextMessage("someone said: " + message);

}
