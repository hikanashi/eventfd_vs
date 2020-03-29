
file(GLOB SOURCES
	${CMAKE_CURRENT_SOURCE_DIR}/*.c*
)

include_directories(${CMAKE_CURRENT_SOURCE_DIR}/include)

add_library(eventfd STATIC
	${SOURCES}
)

find_package(pthreads)
target_link_libraries(eventfd PRIVATE ${PTHREAD_LIBRARY})
target_include_directories(eventfd PRIVATE ${PTHREAD_INCLUDE_DIR})

