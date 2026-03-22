# =============================================================================
# Tests
# =============================================================================
include(GoogleTest)

# test_types — unit tests for Result<T>, VoidResult, ErrorCode
add_executable(test_types test_types.cpp)
target_link_libraries(test_types PRIVATE GTest::gtest_main)
gtest_discover_tests(test_types)

# test_config_manager — unit tests for ConfigManager
add_executable(test_config_manager test_config_manager.cpp)
target_link_libraries(test_config_manager PRIVATE config_manager GTest::gtest_main)
gtest_discover_tests(test_config_manager)

# test_log_manager — unit tests for LogManager
add_executable(test_log_manager test_log_manager.cpp)
target_link_libraries(test_log_manager PRIVATE log_manager GTest::gtest_main)
gtest_discover_tests(test_log_manager)

# test_iot_authenticator — unit tests for IoTAuthenticator
add_executable(test_iot_authenticator test_iot_authenticator.cpp)
target_link_libraries(test_iot_authenticator PRIVATE iot_authenticator GTest::gtest_main)
gtest_discover_tests(test_iot_authenticator)

# test_camera_source — unit tests for Camera_Source
add_executable(test_camera_source test_camera_source.cpp)
target_link_libraries(test_camera_source PRIVATE camera_source GTest::gtest_main)
gtest_discover_tests(test_camera_source)

# test_gstreamer_pipeline — unit tests for GStreamer_Pipeline
add_executable(test_gstreamer_pipeline test_gstreamer_pipeline.cpp)
target_link_libraries(test_gstreamer_pipeline PRIVATE gstreamer_pipeline GTest::gtest_main)
gtest_discover_tests(test_gstreamer_pipeline)

# test_stream_uploader — unit tests for Stream_Uploader
add_executable(test_stream_uploader test_stream_uploader.cpp)
target_link_libraries(test_stream_uploader PRIVATE stream_uploader iot_authenticator GTest::gtest_main)
gtest_discover_tests(test_stream_uploader)

# test_webrtc_agent — unit tests + property tests for WebRTC_Agent
add_executable(test_webrtc_agent test_webrtc_agent.cpp)
target_link_libraries(test_webrtc_agent PRIVATE webrtc_agent iot_authenticator rapidcheck rapidcheck_gtest GTest::gtest_main)
gtest_discover_tests(test_webrtc_agent)

# test_bitrate_controller — unit tests for Bitrate_Controller
add_executable(test_bitrate_controller test_bitrate_controller.cpp)
target_link_libraries(test_bitrate_controller PRIVATE bitrate_controller GTest::gtest_main)
gtest_discover_tests(test_bitrate_controller)

# test_stream_mode_fsm — unit tests for StreamModeFSM
add_executable(test_stream_mode_fsm test_stream_mode_fsm.cpp)
target_link_libraries(test_stream_mode_fsm PRIVATE stream_mode_fsm GTest::gtest_main)
gtest_discover_tests(test_stream_mode_fsm)

# test_connection_monitor — unit tests for ConnectionMonitor
add_executable(test_connection_monitor test_connection_monitor.cpp)
target_link_libraries(test_connection_monitor PRIVATE connection_monitor GTest::gtest_main)
gtest_discover_tests(test_connection_monitor)

# test_health_monitor — unit tests for HealthMonitor
add_executable(test_health_monitor test_health_monitor.cpp)
target_link_libraries(test_health_monitor PRIVATE health_monitor GTest::gtest_main)
gtest_discover_tests(test_health_monitor)

# test_watchdog — unit tests for Watchdog
add_executable(test_watchdog test_watchdog.cpp)
target_link_libraries(test_watchdog PRIVATE watchdog GTest::gtest_main)
gtest_discover_tests(test_watchdog)

# test_frame_buffer_pool — unit tests for Frame_Buffer_Pool
add_executable(test_frame_buffer_pool test_frame_buffer_pool.cpp)
target_link_libraries(test_frame_buffer_pool PRIVATE frame_buffer_pool GTest::gtest_main)
gtest_discover_tests(test_frame_buffer_pool)

# test_video_producer — unit tests for Video_Producer
add_executable(test_video_producer test_video_producer.cpp)
target_link_libraries(test_video_producer PRIVATE video_producer GTest::gtest_main)
gtest_discover_tests(test_video_producer)

# test_ai_pipeline — unit tests for AI_Pipeline
add_executable(test_ai_pipeline test_ai_pipeline.cpp)
target_link_libraries(test_ai_pipeline PRIVATE ai_pipeline GTest::gtest_main)
gtest_discover_tests(test_ai_pipeline)

# test_pipeline_orchestrator — unit tests for Pipeline_Orchestrator
add_executable(test_pipeline_orchestrator test_pipeline_orchestrator.cpp)
target_link_libraries(test_pipeline_orchestrator PRIVATE pipeline_orchestrator GTest::gtest_main)
gtest_discover_tests(test_pipeline_orchestrator)

# test_resource_guard — unit tests for ResourceGuard
add_executable(test_resource_guard test_resource_guard.cpp)
target_link_libraries(test_resource_guard PRIVATE resource_guard GTest::gtest_main)
gtest_discover_tests(test_resource_guard)

# test_shutdown_handler — unit tests for ShutdownHandler
add_executable(test_shutdown_handler test_shutdown_handler.cpp)
target_link_libraries(test_shutdown_handler PRIVATE shutdown_handler GTest::gtest_main)
gtest_discover_tests(test_shutdown_handler)
