/*
* Copyright(c) 2019 Intel Corporation
* Copyright (c) 2019, Alliance for Open Media. All rights reserved
*
* This source code is subject to the terms of the BSD 2 Clause License and
* the Alliance for Open Media Patent License 1.0. If the BSD 2 Clause License
* was not distributed with this source code in the LICENSE file, you can
* obtain it at https://www.aomedia.org/license/software-license. If the Alliance for Open
* Media Patent License 1.0 was not distributed with this source code in the
* PATENTS file, you can obtain it at https://www.aomedia.org/license/patent-license.
*/

#include <stdlib.h>

#include "SystemResourceManager.h"
#include "Definitions.h"
#include "SvtThreads.h"
#if SRM_REPORT
#include "SvtLog.h"
#endif
#include "SvtUtility.h"

static void svt_fifo_dctor(void_ptr p) {
    Fifo_t *obj = (Fifo_t *)p;
    JPEGXS_SVT_DESTROY_SEMAPHORE(obj->counting_semaphore);
    JPEGXS_SVT_DESTROY_MUTEX(obj->lockout_mutex);
}
/**************************************
 * svt_fifo_ctor
 **************************************/
static SvtJxsErrorType_t svt_fifo_ctor(Fifo_t *fifoPtr, uint32_t initial_count, uint32_t max_count,
                                       ObjectWrapper_t *firstWrapperPtr, ObjectWrapper_t *lastWrapperPtr,
                                       MuxingQueue_t *queue_ptr) {
    fifoPtr->dctor = svt_fifo_dctor;
    // Create Counting Semaphore
    JPEGXS_SVT_CREATE_SEMAPHORE(fifoPtr->counting_semaphore, initial_count, max_count);
    if (fifoPtr->counting_semaphore == NULL) {
        return SvtJxsErrorInsufficientResources;
    }

    // Create Buffer Pool Mutex
    SVT_CREATE_MUTEX(fifoPtr->lockout_mutex);
    if (fifoPtr->lockout_mutex == NULL) {
        return SvtJxsErrorInsufficientResources;
    }

    // Initialize Fifo First & Last ptrs
    fifoPtr->first_ptr = firstWrapperPtr;
    fifoPtr->last_ptr = lastWrapperPtr;

    // Copy the Muxing Queue ptr this Fifo belongs to
    fifoPtr->queue_ptr = queue_ptr;

    return SvtJxsErrorNone;
}

/**************************************
 * svt_fifo_push_back
 **************************************/
static SvtJxsErrorType_t svt_fifo_push_back(Fifo_t *fifoPtr, ObjectWrapper_t *wrapper_ptr) {
    SvtJxsErrorType_t return_error = SvtJxsErrorNone;

    // If FIFO is empty
    if (fifoPtr->first_ptr == (ObjectWrapper_t *)NULL) {
        fifoPtr->first_ptr = wrapper_ptr;
        fifoPtr->last_ptr = wrapper_ptr;
    }
    else {
        fifoPtr->last_ptr->next_ptr = wrapper_ptr;
        fifoPtr->last_ptr = wrapper_ptr;
    }

    fifoPtr->last_ptr->next_ptr = (ObjectWrapper_t *)NULL;

    return return_error;
}

/**************************************
 * svt_fifo_pop_front
 **************************************/
static SvtJxsErrorType_t svt_fifo_pop_front(Fifo_t *fifoPtr, ObjectWrapper_t **wrapper_ptr) {
    SvtJxsErrorType_t return_error = SvtJxsErrorNone;

    // Set wrapper_ptr to head of BufferPool
    *wrapper_ptr = fifoPtr->first_ptr;

    // Update tail of BufferPool if the BufferPool is now empty
    fifoPtr->last_ptr = (fifoPtr->first_ptr == fifoPtr->last_ptr) ? (ObjectWrapper_t *)NULL : fifoPtr->last_ptr;

    // Update head of BufferPool
    fifoPtr->first_ptr = fifoPtr->first_ptr->next_ptr;

    return return_error;
}

static SvtJxsErrorType_t svt_fifo_shutdown(Fifo_t *fifo_ptr) {
    SvtJxsErrorType_t return_error = SvtJxsErrorNone;

    // Acquire lockout Mutex
    jpegxs_svt_block_on_mutex(fifo_ptr->lockout_mutex);
    fifo_ptr->quit_signal = 1;
    // Release Mutex
    jpegxs_svt_release_mutex(fifo_ptr->lockout_mutex);
    //Wake up the waiting process if any
    jpegxs_svt_post_semaphore(fifo_ptr->counting_semaphore);

    return return_error;
}

static void svt_circular_buffer_dctor(void_ptr p) {
    CircularBuffer_t *obj = (CircularBuffer_t *)p;
    SVT_FREE(obj->array_ptr);
}

/**************************************
 * svt_circular_buffer_ctor
 **************************************/
static SvtJxsErrorType_t svt_circular_buffer_ctor(CircularBuffer_t *bufferPtr, uint32_t buffer_total_count) {
    bufferPtr->dctor = svt_circular_buffer_dctor;

    bufferPtr->buffer_total_count = buffer_total_count;

    SVT_CALLOC(bufferPtr->array_ptr, bufferPtr->buffer_total_count, sizeof(void_ptr));

    return SvtJxsErrorNone;
}

/**************************************
 * svt_circular_buffer_empty_check
 **************************************/
static uint8_t svt_circular_buffer_empty_check(CircularBuffer_t *bufferPtr) {
    return ((bufferPtr->head_index == bufferPtr->tail_index) && (bufferPtr->array_ptr[bufferPtr->head_index] == NULL));
}

/**************************************
 * svt_circular_buffer_pop_front
 **************************************/
static SvtJxsErrorType_t svt_circular_buffer_pop_front(CircularBuffer_t *bufferPtr, void_ptr *object_ptr) {
    SvtJxsErrorType_t return_error = SvtJxsErrorNone;

    // Copy the head of the buffer into the object_ptr
    *object_ptr = bufferPtr->array_ptr[bufferPtr->head_index];
    bufferPtr->array_ptr[bufferPtr->head_index] = NULL;

    // Increment the head & check for rollover
    bufferPtr->head_index = (bufferPtr->head_index == bufferPtr->buffer_total_count - 1) ? 0 : bufferPtr->head_index + 1;

    // Decrement the Current Count
    --bufferPtr->current_count;

    return return_error;
}

/**************************************
 * svt_circular_buffer_push_back
 **************************************/
static SvtJxsErrorType_t svt_circular_buffer_push_back(CircularBuffer_t *bufferPtr, void_ptr object_ptr) {
    SvtJxsErrorType_t return_error = SvtJxsErrorNone;

    // Copy the pointer into the array
    bufferPtr->array_ptr[bufferPtr->tail_index] = object_ptr;

    // Increment the tail & check for rollover
    bufferPtr->tail_index = (bufferPtr->tail_index == bufferPtr->buffer_total_count - 1) ? 0 : bufferPtr->tail_index + 1;

    // Increment the Current Count
    ++bufferPtr->current_count;

    return return_error;
}

/**************************************
 * svt_circular_buffer_push_front
 **************************************/
static SvtJxsErrorType_t svt_circular_buffer_push_front(CircularBuffer_t *bufferPtr, void_ptr object_ptr) {
    SvtJxsErrorType_t return_error = SvtJxsErrorNone;

    // Decrement the head_index
    bufferPtr->head_index = (bufferPtr->head_index == 0) ? bufferPtr->buffer_total_count - 1 : bufferPtr->head_index - 1;

    // Copy the pointer into the array
    bufferPtr->array_ptr[bufferPtr->head_index] = object_ptr;

    // Increment the Current Count
    ++bufferPtr->current_count;

    return return_error;
}

void jpegxs_svt_muxing_queue_dctor(void_ptr p) {
    MuxingQueue_t *obj = (MuxingQueue_t *)p;
    SVT_DELETE_PTR_ARRAY(obj->process_fifo_ptr_array, obj->process_total_count);
    SVT_DELETE(obj->object_queue);
    SVT_DELETE(obj->process_queue);
    JPEGXS_SVT_DESTROY_MUTEX(obj->lockout_mutex);
}

/**************************************
 * svt_muxing_queue_ctor
 **************************************/
static SvtJxsErrorType_t svt_muxing_queue_ctor(MuxingQueue_t *queue_ptr, uint32_t object_total_count,
                                               uint32_t process_total_count) {
    uint32_t process_index;
    SvtJxsErrorType_t return_error = SvtJxsErrorNone;

    queue_ptr->dctor = jpegxs_svt_muxing_queue_dctor;
    queue_ptr->process_total_count = process_total_count;

    // Lockout Mutex
    SVT_CREATE_MUTEX(queue_ptr->lockout_mutex);
    if (queue_ptr->lockout_mutex == NULL) {
        return SvtJxsErrorCreateMutexFailed;
    }

    // Construct Object Circular Buffer
    SVT_NEW(queue_ptr->object_queue, svt_circular_buffer_ctor, object_total_count);
    // Construct Process Circular Buffer
    SVT_NEW(queue_ptr->process_queue, svt_circular_buffer_ctor, queue_ptr->process_total_count);
    // Construct the Process Fifos
    SVT_ALLOC_PTR_ARRAY(queue_ptr->process_fifo_ptr_array, queue_ptr->process_total_count);

    for (process_index = 0; process_index < queue_ptr->process_total_count; ++process_index) {
        SVT_NEW(queue_ptr->process_fifo_ptr_array[process_index],
                svt_fifo_ctor,
                0,
                object_total_count,
                (ObjectWrapper_t *)NULL,
                (ObjectWrapper_t *)NULL,
                queue_ptr);
    }

    return return_error;
}

/**************************************
 * svt_muxing_queue_assignation
 **************************************/
static SvtJxsErrorType_t svt_muxing_queue_assignation(MuxingQueue_t *queue_ptr) {
    SvtJxsErrorType_t return_error = SvtJxsErrorNone;
    Fifo_t *process_fifo_ptr;
    ObjectWrapper_t *wrapper_ptr;

    // while loop
    while ((!svt_circular_buffer_empty_check(queue_ptr->object_queue)) &&
           (!svt_circular_buffer_empty_check(queue_ptr->process_queue))) {
        // Get the next process
        svt_circular_buffer_pop_front(queue_ptr->process_queue, (void **)&process_fifo_ptr);

        // Get the next object
        svt_circular_buffer_pop_front(queue_ptr->object_queue, (void **)&wrapper_ptr);

        // Block on the Process Fifo's Mutex
        jpegxs_svt_block_on_mutex(process_fifo_ptr->lockout_mutex);

        // Put the object on the fifo
        svt_fifo_push_back(process_fifo_ptr, wrapper_ptr);

        // Release the Process Fifo's Mutex
        jpegxs_svt_release_mutex(process_fifo_ptr->lockout_mutex);

        // Post the semaphore
        jpegxs_svt_post_semaphore(process_fifo_ptr->counting_semaphore);
    }

    return return_error;
}

/**************************************
 * svt_muxing_queue_object_push_back
 **************************************/
static SvtJxsErrorType_t svt_muxing_queue_object_push_back(MuxingQueue_t *queue_ptr, ObjectWrapper_t *object_ptr) {
    SvtJxsErrorType_t return_error = SvtJxsErrorNone;

    svt_circular_buffer_push_back(queue_ptr->object_queue, object_ptr);

    svt_muxing_queue_assignation(queue_ptr);

    return return_error;
}

/**************************************
* svt_muxing_queue_object_push_front
**************************************/
static SvtJxsErrorType_t svt_muxing_queue_object_push_front(MuxingQueue_t *queue_ptr, ObjectWrapper_t *object_ptr) {
    SvtJxsErrorType_t return_error = SvtJxsErrorNone;

    svt_circular_buffer_push_front(queue_ptr->object_queue, object_ptr);

    svt_muxing_queue_assignation(queue_ptr);

    return return_error;
}

static Fifo_t *svt_muxing_queue_get_fifo(MuxingQueue_t *queue_ptr, uint32_t index) {
    assert(queue_ptr->process_fifo_ptr_array && (queue_ptr->process_total_count > index));
    return queue_ptr->process_fifo_ptr_array[index];
}

/*********************************************************************
 * jpegxs_svt_object_release_enable
 *   Enables the release_enable member of ObjectWrapper_t.  Used by
 *   certain objects (e.g. SequenceControlSet) to control whether
 *   ObjectWrapper_ts are allowed to be released or not.
 *
 *   resource_ptr
 *      pointer to the SystemResource that manages the ObjectWrapper_t.
 *      The emptyFifo's lockout_mutex is used to write protect the
 *      modification of the ObjectWrapper_t.
 *
 *   wrapper_ptr
 *      pointer to the ObjectWrapper_t to be modified.
 *********************************************************************/
SvtJxsErrorType_t jpegxs_svt_object_release_enable(ObjectWrapper_t *wrapper_ptr) {
    SvtJxsErrorType_t return_error = SvtJxsErrorNone;

    jpegxs_svt_block_on_mutex(wrapper_ptr->system_resource_ptr->empty_queue->lockout_mutex);

    wrapper_ptr->release_enable = 1;

    jpegxs_svt_release_mutex(wrapper_ptr->system_resource_ptr->empty_queue->lockout_mutex);

    return return_error;
}

/*********************************************************************
 * jpegxs_svt_object_release_disable
 *   Disables the release_enable member of ObjectWrapper_t.  Used by
 *   certain objects (e.g. SequenceControlSet) to control whether
 *   ObjectWrapper_ts are allowed to be released or not.
 *
 *   resource_ptr
 *      pointer to the SystemResource that manages the ObjectWrapper_t.
 *      The emptyFifo's lockout_mutex is used to write protect the
 *      modification of the ObjectWrapper_t.
 *
 *   wrapper_ptr
 *      pointer to the ObjectWrapper_t to be modified.
 *********************************************************************/
SvtJxsErrorType_t jpegxs_svt_object_release_disable(ObjectWrapper_t *wrapper_ptr) {
    SvtJxsErrorType_t return_error = SvtJxsErrorNone;

    jpegxs_svt_block_on_mutex(wrapper_ptr->system_resource_ptr->empty_queue->lockout_mutex);

    wrapper_ptr->release_enable = 0;

    jpegxs_svt_release_mutex(wrapper_ptr->system_resource_ptr->empty_queue->lockout_mutex);

    return return_error;
}

/*********************************************************************
 * jpegxs_svt_object_inc_live_count
 *   Increments the live_count member of ObjectWrapper_t.  Used by
 *   certain objects (e.g. SequenceControlSet) to count the number of active
 *   pointers of a ObjectWrapper_t in pipeline at any point in time.
 *
 *   resource_ptr
 *      pointer to the SystemResource that manages the ObjectWrapper_t.
 *      The emptyFifo's lockout_mutex is used to write protect the
 *      modification of the ObjectWrapper_t.
 *
 *   wrapper_ptr
 *      pointer to the ObjectWrapper_t to be modified.
 *********************************************************************/
SvtJxsErrorType_t jpegxs_svt_object_inc_live_count(ObjectWrapper_t *wrapper_ptr, uint32_t increment_number) {
    SvtJxsErrorType_t return_error = SvtJxsErrorNone;

    jpegxs_svt_block_on_mutex(wrapper_ptr->system_resource_ptr->empty_queue->lockout_mutex);

    assert_err(wrapper_ptr->live_count != ObjectWrapperReleasedValue,
               "live_count should not be ObjectWrapperReleasedValue when inc");

    wrapper_ptr->live_count += increment_number;

    jpegxs_svt_release_mutex(wrapper_ptr->system_resource_ptr->empty_queue->lockout_mutex);

    return return_error;
}

//ugly hack
typedef struct DctorAble {
    DctorCall dctor;
} DctorAble;

void jpegxs_svt_object_wrapper_dctor(void_ptr p) {
    ObjectWrapper_t *wrapper = (ObjectWrapper_t *)p;
    if (wrapper->object_destroyer) {
        //customized destructor
        if (wrapper->object_ptr)
            wrapper->object_destroyer(wrapper->object_ptr);
    }
    else {
        //hack....
        DctorAble *obj = (DctorAble *)wrapper->object_ptr;
        SVT_DELETE(obj);
    }
}

static SvtJxsErrorType_t svt_object_wrapper_ctor(ObjectWrapper_t *wrapper, SystemResource_t *resource, Creator_t object_creator,
                                                 void_ptr object_init_data_ptr, DctorCall object_destroyer) {
    SvtJxsErrorType_t ret;

    wrapper->dctor = jpegxs_svt_object_wrapper_dctor;
    wrapper->release_enable = 1;
    wrapper->system_resource_ptr = resource;
    wrapper->object_destroyer = object_destroyer;
    ret = object_creator(&wrapper->object_ptr, object_init_data_ptr);
    if (ret != SvtJxsErrorNone)
        return ret;
    return SvtJxsErrorNone;
}

static void svt_system_resource_dctor(void_ptr p) {
    SystemResource_t *obj = (SystemResource_t *)p;
    SVT_DELETE(obj->full_queue);
    SVT_DELETE(obj->empty_queue);
    SVT_DELETE_PTR_ARRAY(obj->wrapper_ptr_pool, obj->object_total_count);
}

/*********************************************************************
 * jpegxs_svt_system_resource_ctor
 *   Constructor for SystemResource_t.  Fully constructs all members
 *   of SystemResource_t including the object with the passed
 *   object_ctor function.
 *
 *   resource_ptr
 *     pointer that will contain the SystemResource to be constructed.
 *
 *   object_total_count
 *     Number of objects to be managed by the SystemResource.
 *
 *   object_ctor
 *     Function pointer to the constructor of the object managed by
 *     SystemResource referenced by resource_ptr. No object level
 *     construction is performed if object_ctor is NULL.
 *
 *   object_init_data_ptr

 *     pointer to data block to be used during the construction of
 *     the object. object_init_data_ptr is passed to object_ctor when
 *     object_ctor is called.
 *   object_destroyer
 *     object destroyer, will call dctor if this is null
 *********************************************************************/
SvtJxsErrorType_t jpegxs_svt_system_resource_ctor(SystemResource_t *resource_ptr, uint32_t object_total_count,
                                           uint32_t producer_process_total_count, uint32_t consumer_process_total_count,
                                           Creator_t object_creator, void_ptr object_init_data_ptr, DctorCall object_destroyer) {
    uint32_t wrapper_index;
    SvtJxsErrorType_t return_error = SvtJxsErrorNone;
    resource_ptr->dctor = svt_system_resource_dctor;

    resource_ptr->object_total_count = object_total_count;

    // Allocate array for wrapper pointers
    SVT_ALLOC_PTR_ARRAY(resource_ptr->wrapper_ptr_pool, resource_ptr->object_total_count);

    // Initialize each wrapper
    for (wrapper_index = 0; wrapper_index < resource_ptr->object_total_count; ++wrapper_index) {
        SVT_NEW(resource_ptr->wrapper_ptr_pool[wrapper_index],
                svt_object_wrapper_ctor,
                resource_ptr,
                object_creator,
                object_init_data_ptr,
                object_destroyer);

#if SRM_REPORT
        resource_ptr->wrapper_ptr_pool[wrapper_index]->pic_number = 99999999;
#endif
    }

    // Initialize the Empty Queue
    SVT_NEW(resource_ptr->empty_queue, svt_muxing_queue_ctor, resource_ptr->object_total_count, producer_process_total_count);
    // Fill the Empty Fifo with every ObjectWrapper
    for (wrapper_index = 0; wrapper_index < resource_ptr->object_total_count; ++wrapper_index) {
        svt_muxing_queue_object_push_back(resource_ptr->empty_queue, resource_ptr->wrapper_ptr_pool[wrapper_index]);
    }
#if SRM_REPORT
    //at init time, the SRM is full
    resource_ptr->empty_queue->curr_count = resource_ptr->object_total_count;
    resource_ptr->empty_queue->log = 0;
#endif
    // Initialize the Full Queue
    if (consumer_process_total_count) {
        SVT_NEW(resource_ptr->full_queue, svt_muxing_queue_ctor, resource_ptr->object_total_count, consumer_process_total_count);
    }
    else {
        resource_ptr->full_queue = (MuxingQueue_t *)NULL;
    }

    return return_error;
}

Fifo_t *jpegxs_svt_system_resource_get_producer_fifo(const SystemResource_t *resource_ptr, uint32_t index) {
    return svt_muxing_queue_get_fifo(resource_ptr->empty_queue, index);
}

Fifo_t *jpegxs_svt_system_resource_get_consumer_fifo(const SystemResource_t *resource_ptr, uint32_t index) {
    return svt_muxing_queue_get_fifo(resource_ptr->full_queue, index);
}

SvtJxsErrorType_t jpegxs_svt_shutdown_process(const SystemResource_t *resource_ptr) {
    //not fully constructed
    if (!resource_ptr || !resource_ptr->full_queue)
        return SvtJxsErrorNone;

    //notify all consumers we are shutting down
    for (unsigned int i = 0; i < resource_ptr->full_queue->process_total_count; i++) {
        Fifo_t *fifo_ptr = jpegxs_svt_system_resource_get_consumer_fifo(resource_ptr, i);
        svt_fifo_shutdown(fifo_ptr);
    }
    return SvtJxsErrorNone;
}

/*********************************************************************
 * SystemResource_tReleaseProcess
 *********************************************************************/
static SvtJxsErrorType_t svt_release_process(Fifo_t *process_fifo_ptr) {
    SvtJxsErrorType_t return_error = SvtJxsErrorNone;

    jpegxs_svt_block_on_mutex(process_fifo_ptr->queue_ptr->lockout_mutex);

    svt_circular_buffer_push_front(process_fifo_ptr->queue_ptr->process_queue, process_fifo_ptr);

    svt_muxing_queue_assignation(process_fifo_ptr->queue_ptr);

    jpegxs_svt_release_mutex(process_fifo_ptr->queue_ptr->lockout_mutex);

    return return_error;
}

/*********************************************************************
 * SystemResource_tPostObject
 *   Queues a full ObjectWrapper_t to the SystemResource. This
 *   function posts the SystemResource fullFifo counting_semaphore.
 *   This function is write protected by the SystemResource fullFifo
 *   lockout_mutex.
 *
 *   resource_ptr
 *      pointer to the SystemResource that the ObjectWrapper_t is
 *      posted to.
 *
 *   wrapper_ptr
 *      pointer to ObjectWrapper_t to be posted.
 *********************************************************************/
SvtJxsErrorType_t jpegxs_svt_post_full_object(ObjectWrapper_t *object_ptr) {
    SvtJxsErrorType_t return_error = SvtJxsErrorNone;

    jpegxs_svt_block_on_mutex(object_ptr->system_resource_ptr->full_queue->lockout_mutex);

    svt_muxing_queue_object_push_back(object_ptr->system_resource_ptr->full_queue, object_ptr);

    jpegxs_svt_release_mutex(object_ptr->system_resource_ptr->full_queue->lockout_mutex);

    return return_error;
}

/*********************************************************************
 * SystemResource_tReleaseObject
 *   Queues an empty ObjectWrapper_t to the SystemResource. This
 *   function posts the SystemResource emptyFifo counting_semaphore.
 *   This function is write protected by the SystemResource emptyFifo
 *   lockout_mutex.
 *
 *   object_ptr
 *      pointer to ObjectWrapper_t to be released.
 *********************************************************************/
SvtJxsErrorType_t jpegxs_svt_release_object(ObjectWrapper_t *object_ptr) {
    SvtJxsErrorType_t return_error = SvtJxsErrorNone;

    jpegxs_svt_block_on_mutex(object_ptr->system_resource_ptr->empty_queue->lockout_mutex);

    assert_err(object_ptr->live_count != ObjectWrapperReleasedValue,
               "live_count should not be ObjectWrapperReleasedValue when release");

    // Decrement live_count
    object_ptr->live_count = (object_ptr->live_count == 0) ? object_ptr->live_count : object_ptr->live_count - 1;

    if ((object_ptr->release_enable == 1) && (object_ptr->live_count == 0)) {
        // Set live_count to ObjectWrapperReleasedValue
        object_ptr->live_count = ObjectWrapperReleasedValue;

        svt_muxing_queue_object_push_front(object_ptr->system_resource_ptr->empty_queue, object_ptr);
#if SRM_REPORT
        object_ptr->pic_number = 99999999;
        //increment the fullness
        object_ptr->system_resource_ptr->empty_queue->curr_count++;
        if (object_ptr->system_resource_ptr->empty_queue->log)
            SVT_LOG("SRM fullness+: %i/%i\n",
                    object_ptr->system_resource_ptr->empty_queue->curr_count,
                    object_ptr->system_resource_ptr->object_total_count);
#endif
    }

    jpegxs_svt_release_mutex(object_ptr->system_resource_ptr->empty_queue->lockout_mutex);

    return return_error;
}

SvtJxsErrorType_t jpegxs_svt_release_dual_object(ObjectWrapper_t *object_ptr, ObjectWrapper_t *sec_object_ptr) {
    SvtJxsErrorType_t return_error = SvtJxsErrorNone;

    jpegxs_svt_block_on_mutex(object_ptr->system_resource_ptr->empty_queue->lockout_mutex);

    // Decrement live_count
    object_ptr->live_count = (object_ptr->live_count == 0) ? object_ptr->live_count : object_ptr->live_count - 1;

    if ((object_ptr->release_enable == 1) && (object_ptr->live_count == 0)) {
        //release the second object
        jpegxs_svt_release_object(sec_object_ptr);

        // Set live_count to ObjectWrapperReleasedValue
        object_ptr->live_count = ObjectWrapperReleasedValue;

        svt_muxing_queue_object_push_front(object_ptr->system_resource_ptr->empty_queue, object_ptr);

#if SRM_REPORT

        if (object_ptr->system_resource_ptr->empty_queue->log)
            SVT_LOG("SRM RELEASE: %lld\n", object_ptr->pic_number);

        object_ptr->pic_number = 99999999;
        //increment the fullness
        object_ptr->system_resource_ptr->empty_queue->curr_count++;
        //  if (object_ptr->system_resource_ptr->empty_queue->log)
        //      SVT_LOG("SRM fullness+: %i/%i\n", object_ptr->system_resource_ptr->empty_queue->curr_count, object_ptr->system_resource_ptr->object_total_count);
#endif
    }

    jpegxs_svt_release_mutex(object_ptr->system_resource_ptr->empty_queue->lockout_mutex);

    return return_error;
}
#if SRM_REPORT
/*
  dump pictures occupying the SRM
*/
SvtJxsErrorType_t dump_srm_content(SystemResource_t *resource_ptr, uint8_t log) {
    SvtJxsErrorType_t return_error = SvtJxsErrorNone;
    if (log) {
        SVT_LOG("SRM content:\n\n");
        for (uint32_t wrapper_index = 0; wrapper_index < resource_ptr->object_total_count; ++wrapper_index) {
            SVT_LOG("%lld ", resource_ptr->wrapper_ptr_pool[wrapper_index]->pic_number);
        }
    }
    return return_error;
}
#endif

/**************************************
* svt_fifo_pop_front
**************************************/
static uint8_t svt_fifo_peak_front(Fifo_t *fifoPtr) {
    // Set wrapper_ptr to head of BufferPool
    return (fifoPtr->first_ptr == (ObjectWrapper_t *)NULL);
}

/*********************************************************************
 * SystemResource_tGetEmptyObject
 *   Dequeues an empty ObjectWrapper_t from the SystemResource.  This
 *   function blocks on the SystemResource emptyFifo counting_semaphore.
 *   This function is write protected by the SystemResource emptyFifo
 *   lockout_mutex.
 *
 *   resource_ptr
 *      pointer to the SystemResource that provides the empty
 *      ObjectWrapper_t.
 *
 *   wrapper_dbl_ptr
 *      Double pointer used to pass the pointer to the empty
 *      ObjectWrapper_t pointer.
 *********************************************************************/
SvtJxsErrorType_t jpegxs_svt_get_empty_object(Fifo_t *empty_fifo_ptr, ObjectWrapper_t **wrapper_dbl_ptr) {
    SvtJxsErrorType_t return_error = SvtJxsErrorNone;

    // Queue the Fifo requesting the empty fifo
    svt_release_process(empty_fifo_ptr);

    // Block on the counting Semaphore until an empty buffer is available
    jpegxs_svt_block_on_semaphore(empty_fifo_ptr->counting_semaphore);

    // Acquire lockout Mutex
    jpegxs_svt_block_on_mutex(empty_fifo_ptr->lockout_mutex);

    // Get the empty object
    svt_fifo_pop_front(empty_fifo_ptr, wrapper_dbl_ptr);

#if SRM_REPORT
    //decrement the fullness
    empty_fifo_ptr->queue_ptr->curr_count--;
    if (empty_fifo_ptr->queue_ptr->log)
        printf("SRM fullness-: %i/%i\n",
               empty_fifo_ptr->queue_ptr->curr_count,
               (*wrapper_dbl_ptr)->system_resource_ptr->object_total_count);
#endif

    assert_err((*wrapper_dbl_ptr)->live_count == 0 || (*wrapper_dbl_ptr)->live_count == ObjectWrapperReleasedValue,
               "live_count should be 0 or ObjectWrapperReleasedValue when get");

    // Reset the wrapper's live_count
    (*wrapper_dbl_ptr)->live_count = 0;

    // Object release enable
    (*wrapper_dbl_ptr)->release_enable = 1;

    // Release Mutex
    jpegxs_svt_release_mutex(empty_fifo_ptr->lockout_mutex);

    return return_error;
}

SvtJxsErrorType_t jpegxs_svt_get_empty_object_non_blocking(Fifo_t *empty_fifo_ptr, ObjectWrapper_t **wrapper_dbl_ptr) {
    SvtJxsErrorType_t return_error = SvtJxsErrorNone;

    // Queue the Fifo requesting the empty fifo
    svt_release_process(empty_fifo_ptr);

    // Acquire lockout Mutex
    jpegxs_svt_block_on_mutex(empty_fifo_ptr->lockout_mutex);

    uint8_t empty_flag = svt_fifo_peak_front(empty_fifo_ptr);

    if (!empty_flag) {
        // Block on the counting Semaphore until an empty buffer is available
        jpegxs_svt_block_on_semaphore(empty_fifo_ptr->counting_semaphore);

        // Get the empty object
        svt_fifo_pop_front(empty_fifo_ptr, wrapper_dbl_ptr);

#if SRM_REPORT
        //decrement the fullness
        empty_fifo_ptr->queue_ptr->curr_count--;
        if (empty_fifo_ptr->queue_ptr->log)
            printf("SRM fullness-: %i/%i\n",
                   empty_fifo_ptr->queue_ptr->curr_count,
                   (*wrapper_dbl_ptr)->system_resource_ptr->object_total_count);
#endif

        assert_err((*wrapper_dbl_ptr)->live_count == 0 || (*wrapper_dbl_ptr)->live_count == ObjectWrapperReleasedValue,
                   "live_count should be 0 or ObjectWrapperReleasedValue when get");

        // Reset the wrapper's live_count
        (*wrapper_dbl_ptr)->live_count = 0;

        // Object release enable
        (*wrapper_dbl_ptr)->release_enable = 1;
    }
    else {
        *wrapper_dbl_ptr = NULL;
    }

    // Release Mutex
    jpegxs_svt_release_mutex(empty_fifo_ptr->lockout_mutex);

    return return_error;
}

/*********************************************************************
 * SystemResource_tGetFullObject
 *   Dequeues an full ObjectWrapper_t from the SystemResource. This
 *   function blocks on the SystemResource fullFifo counting_semaphore.
 *   This function is write protected by the SystemResource fullFifo
 *   lockout_mutex.
 *
 *   resource_ptr
 *      pointer to the SystemResource that provides the full
 *      ObjectWrapper_t.
 *
 *   wrapper_dbl_ptr
 *      Double pointer used to pass the pointer to the full
 *      ObjectWrapper_t pointer.
 *********************************************************************/
SvtJxsErrorType_t jpegxl_svt_get_full_object(Fifo_t *full_fifo_ptr, ObjectWrapper_t **wrapper_dbl_ptr) {
    SvtJxsErrorType_t return_error = SvtJxsErrorNone;

    // Queue the Fifo requesting the full fifo
    svt_release_process(full_fifo_ptr);

    // Block on the counting Semaphore until an empty buffer is available
    jpegxs_svt_block_on_semaphore(full_fifo_ptr->counting_semaphore);

    // Acquire lockout Mutex
    jpegxs_svt_block_on_mutex(full_fifo_ptr->lockout_mutex);

    if (!full_fifo_ptr->quit_signal) {
        svt_fifo_pop_front(full_fifo_ptr, wrapper_dbl_ptr);
    }
    else {
        *wrapper_dbl_ptr = NULL;
        return_error = SvtJxsErrorNoErrorFifoShutdown;
    }

    // Release Mutex
    jpegxs_svt_release_mutex(full_fifo_ptr->lockout_mutex);

    return return_error;
}

SvtJxsErrorType_t jpegxs_svt_get_full_object_non_blocking(Fifo_t *full_fifo_ptr, ObjectWrapper_t **wrapper_dbl_ptr) {
    SvtJxsErrorType_t return_error = SvtJxsErrorNone;
    uint8_t fifo_empty;
    // Queue the Fifo requesting the full fifo
    svt_release_process(full_fifo_ptr);

    // Acquire lockout Mutex
    jpegxs_svt_block_on_mutex(full_fifo_ptr->lockout_mutex);

    //if the fifo is shutting down, we will not give any buffer to caller
    if (!full_fifo_ptr->quit_signal)
        fifo_empty = svt_fifo_peak_front(full_fifo_ptr);
    else
        fifo_empty = 1;

    // Release Mutex
    jpegxs_svt_release_mutex(full_fifo_ptr->lockout_mutex);

    if (!fifo_empty)
        jpegxl_svt_get_full_object(full_fifo_ptr, wrapper_dbl_ptr);
    else
        *wrapper_dbl_ptr = (ObjectWrapper_t *)NULL;

    return return_error;
}
