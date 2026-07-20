if(BNIO_PLATFORM_LINUX OR BNIO_PLATFORM_BSD)
  add_subdirectory(throughput)
  add_subdirectory(timer_churn)
endif()
