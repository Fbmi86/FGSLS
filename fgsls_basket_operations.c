/*
 * fgsls_basket_operations.c - Basket management and operations for FGSLS
 * This file handles all Basket-related operations for storing small files
 */

#include "fgsls.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

// Forward declarations
static int _fgsls_allocate_basket_space(fgsls_system_t *system, uint16_t shelf_id, 
                                       uint32_t basket_size, uint64_t *physical_offset);
static int _fgsls_write_basket_header(fgsls_system_t *system, const fgsls_basket_header_t *header);
static int _fgsls_read_basket_header(fgsls_system_t *system, const fgsls_tag_t *tag, 
                                    fgsls_basket_header_t *header);
static int _fgsls_find_free_file_slot(const fgsls_basket_header_t *header, uint32_t *slot_index);
static int _fgsls_compact_basket(fgsls_system_t *system, fgsls_basket_header_t *header);
static int _fgsls_update_basket_position(fgsls_system_t *system, const fgsls_tag_t *basket_tag,
                                        const fgsls_tag_t *file_tag, uint16_t shelf_id, 
                                        uint64_t physical_offset, uint32_t internal_offset);

/**
 * Create a new Basket
 */
int fgsls_create_basket(fgsls_system_t *system, uint16_t shelf_id, fgsls_tag_t *tag) {
    FGSLS_TRACE_ENTER("fgsls_create_basket");
    
    if (!system || !tag || shelf_id >= system->shelf_count) {
        return FGSLS_ERROR_INVALID_PARAMETER;
    }
    
    if (!system->is_mounted) {
        return FGSLS_ERROR_SYSTEM_NOT_MOUNTED;
    }
    
    // Check shelf capacity
    fgsls_shelf_header_t *shelf = &system->shelves[shelf_id];
    if (shelf->config.basket_count >= shelf->config.max_baskets) {
        FGSLS_DEBUG_PRINT("Shelf %d is full (baskets: %d/%d)", 
                          shelf_id, shelf->config.basket_count, shelf->config.max_baskets);
        return FGSLS_ERROR_SHELF_FULL;
    }
    
    if (shelf->config.used_size + BASKET_DEFAULT_SIZE > shelf->config.total_size) {
        FGSLS_DEBUG_PRINT("Not enough space in shelf %d for basket", shelf_id);
        return FGSLS_ERROR_DISK_FULL;
    }
    
    // Generate unique tag
    *tag = fgsls_generate_tag();
    
    // Allocate physical space
    uint64_t physical_offset;
    int result = _fgsls_allocate_basket_space(system, shelf_id, BASKET_DEFAULT_SIZE, 
                                             &physical_offset);
    if (result != FGSLS_SUCCESS) {
        return result;
    }
    
    // Create basket header
    fgsls_basket_header_t header;
    memset(&header, 0, sizeof(header));
    
    fgsls_copy_tag(&header.tag, tag);
    header.shelf_id = shelf_id;
    header.basket_size = BASKET_DEFAULT_SIZE;
    header.file_count = 0;
    header.deleted_count = 0;
    header.used_space = sizeof(fgsls_basket_header_t); // Header takes space
    header.free_space = BASKET_DEFAULT_SIZE - header.used_space;
    header.creation_time = fgsls_get_current_time();
    header.last_compaction = header.creation_time;
    header.compaction_count = 0;
    header.physical_offset = physical_offset;
    
    // Initialize all file entries as unused
    for (int i = 0; i < BASKET_MAX_FILES; i++) {
        memset(&header.files[i], 0, sizeof(fgsls_basket_file_entry_t));
        header.files[i].is_deleted = true; // Mark as unused
    }
    
    // Write basket header
    result = _fgsls_write_basket_header(system, &header);
    if (result != FGSLS_SUCCESS) {
        return result;
    }
    
    // Update Taver index
    result = _fgsls_update_basket_position(system, tag, tag, shelf_id, physical_offset, 0);
    if (result != FGSLS_SUCCESS) {
        return result;
    }
    
    // Update shelf statistics
    shelf->config.basket_count++;
    shelf->config.used_size += BASKET_DEFAULT_SIZE;
    shelf->config.free_size = shelf->config.total_size - shelf->config.used_size;
    
    // Log journal entry
    sant_journal_entry_t journal_entry;
    memset(&journal_entry, 0, sizeof(journal_entry));
    journal_entry.sequence_number = system->total_writes++;
    journal_entry.timestamp = fgsls_get_current_time();
    journal_entry.operation_type = JOURNAL_WRITE;
    fgsls_copy_tag(&journal_entry.target_tag, tag);
    journal_entry.shelf_id = shelf_id;
    journal_entry.data_size = BASKET_DEFAULT_SIZE;
    snprintf(journal_entry.description, sizeof(journal_entry.description),
             "Created basket on shelf %d", shelf_id);
    
    fgsls_write_journal_entry(system, JOURNAL_WAREHOUSING_ENGINE, &journal_entry, sizeof(journal_entry));
    
    FGSLS_DEBUG_PRINT("Created basket on shelf %d", shelf_id);
    FGSLS_TRACE_EXIT("fgsls_create_basket", FGSLS_SUCCESS);
    return FGSLS_SUCCESS;
}

/**
 * Add a file to a Basket
 */
int fgsls_add_file_to_basket(fgsls_system_t *system, const fgsls_tag_t *basket_tag,
                             const char *filename, const void *data, uint32_t size,
                             fgsls_tag_t *file_tag) {
    FGSLS_TRACE_ENTER("fgsls_add_file_to_basket");
    
    if (!system || !basket_tag || !filename || !data || size == 0 || !file_tag) {
        return FGSLS_ERROR_INVALID_PARAMETER;
    }
    
    if (!system->is_mounted) {
        return FGSLS_ERROR_SYSTEM_NOT_MOUNTED;
    }
    
    // Check file size limit for baskets
    if (size > BASKET_MAX_FILE_SIZE) {
        FGSLS_DEBUG_PRINT("File size %u exceeds basket limit %d", size, BASKET_MAX_FILE_SIZE);
        return FGSLS_ERROR_INVALID_PARAMETER;
    }
    
    // Check filename length
    if (strlen(filename) >= MAX_FILENAME_LENGTH) {
        FGSLS_DEBUG_PRINT("Filename too long: %zu characters", strlen(filename));
        return FGSLS_ERROR_INVALID_PARAMETER;
    }
    
    // Read basket header
    fgsls_basket_header_t header;
    int result = _fgsls_read_basket_header(system, basket_tag, &header);
    if (result != FGSLS_SUCCESS) {
        return result;
    }
    
    // Check if basket has space for another file
    if (header.file_count >= BASKET_MAX_FILES) {
        FGSLS_DEBUG_PRINT("Basket is full (files: %d/%d)", header.file_count, BASKET_MAX_FILES);
        return FGSLS_ERROR_BASKET_FULL;
    }
    
    // Check if basket has enough free space
    if (header.free_space < size) {
        // Try compaction first
        result = _fgsls_compact_basket(system, &header);
        if (result != FGSLS_SUCCESS || header.free_space < size) {
            FGSLS_DEBUG_PRINT("Not enough space in basket (need: %u, available: %llu)", 
                              size, (unsigned long long)header.free_space);
            return FGSLS_ERROR_BASKET_FULL;
        }
    }
    
    // Find free file slot
    uint32_t slot_index;
    result = _fgsls_find_free_file_slot(&header, &slot_index);
    if (result != FGSLS_SUCCESS) {
        return result;
    }
    
    // Generate file tag
    *file_tag = fgsls_generate_tag();
    
    // Create file entry
    fgsls_basket_file_entry_t *file_entry = &header.files[slot_index];
    memset(file_entry, 0, sizeof(*file_entry));
    
    fgsls_copy_tag(&file_entry->tag, file_tag);
    strncpy(file_entry->filename, filename, sizeof(file_entry->filename) - 1);
    file_entry->file_size = size;
    file_entry->data_offset = header.basket_size - header.free_space; // Add to end
    file_entry->creation_time = fgsls_get_current_time();
    file_entry->modification_time = file_entry->creation_time;
    file_entry->access_time = file_entry->creation_time;
    file_entry->data_type = DATA_TYPE_UNKNOWN; // Could be detected from filename
    file_entry->permissions = 0644; // Default permissions
    file_entry->is_deleted = false;
    
    // Calculate file hash
    fgsls_calculate_hash(data, size, &file_entry->file_hash);
    
    // TODO: Write file data to basket at data_offset
    // In real implementation, this would write to physical storage
    
    // Update basket header
    header.file_count++;
    header.used_space += size;
    header.free_space -= size;
    
    // Calculate basket hash
    fgsls_basket_header_t temp_header = header;
    memset(&temp_header.basket_hash, 0, sizeof(temp_header.basket_hash));
    fgsls_calculate_hash(&temp_header, sizeof(temp_header), &header.basket_hash);
    
    // Write updated basket header
    result = _fgsls_write_basket_header(system, &header);
    if (result != FGSLS_SUCCESS) {
        return result;
    }
    
    // Update Taver index for the file
    result = _fgsls_update_basket_position(system, basket_tag, file_tag, header.shelf_id,
                                          header.physical_offset, file_entry->data_offset);
    if (result != FGSLS_SUCCESS) {
        return result;
    }
    
    // Log journal entry
    sant_journal_entry_t journal_entry;
    memset(&journal_entry, 0, sizeof(journal_entry));
    journal_entry.sequence_number = system->total_writes++;
    journal_entry.timestamp = fgsls_get_current_time();
    journal_entry.operation_type = JOURNAL_WRITE;
    fgsls_copy_tag(&journal_entry.target_tag, file_tag);
    journal_entry.shelf_id = header.shelf_id;
    journal_entry.data_size = size;
    snprintf(journal_entry.description, sizeof(journal_entry.description),
             "Added file '%s' (%u bytes) to basket", filename, size);
    
    fgsls_write_journal_entry(system, JOURNAL_WAREHOUSING_ENGINE, &journal_entry, sizeof(journal_entry));
    
    FGSLS_DEBUG_PRINT("Added file '%s' (%u bytes) to basket on shelf %d", 
                      filename, size, header.shelf_id);
    FGSLS_TRACE_EXIT("fgsls_add_file_to_basket", FGSLS_SUCCESS);
    return FGSLS_SUCCESS;
}

/**
 * Read a file from a Basket
 */
int fgsls_read_file_from_basket(fgsls_system_t *system, const fgsls_tag_t *file_tag,
                                void *buffer, uint32_t *size) {
    FGSLS_TRACE_ENTER("fgsls_read_file_from_basket");
    
    if (!system || !file_tag || !buffer || !size) {
        return FGSLS_ERROR_INVALID_PARAMETER;
    }
    
    if (!system->is_mounted) {
        return FGSLS_ERROR_SYSTEM_NOT_MOUNTED;
    }
    
    // Find file location using Taver
    fgsls_taver_index_t *taver = &system->taver_index;
    fgsls_position_entry_t *entry = NULL;
    
    for (uint32_t i = 0; i < taver->entry_count; i++) {
        if (fgsls_compare_tags(&taver->entries[i].tag, file_tag) == 0) {
            entry = &taver->entries[i];
            break;
        }
    }
    
    if (!entry || entry->container_type != CONTAINER_BASKET_FILE) {
        return FGSLS_ERROR_FILE_NOT_FOUND;
    }
    
    // Find basket tag (we need to iterate to find which basket contains this file)
    fgsls_tag_t basket_tag;
    bool found_basket = false;
    
    for (uint32_t i = 0; i < taver->entry_count; i++) {
        if (taver->entries[i].container_type == CONTAINER_BASKET &&
            taver->entries[i].shelf_id == entry->shelf_id &&
            taver->entries[i].physical_offset == entry->physical_offset) {
            fgsls_copy_tag(&basket_tag, &taver->entries[i].tag);
            found_basket = true;
            break;
        }
    }
    
    if (!found_basket) {
        return FGSLS_ERROR_CORRUPTED_DATA;
    }
    
    // Read basket header
    fgsls_basket_header_t header;
    int result = _fgsls_read_basket_header(system, &basket_tag, &header);
    if (result != FGSLS_SUCCESS) {
        return result;
    }
    
    // Find file entry in basket
    fgsls_basket_file_entry_t *file_entry = NULL;
    for (uint32_t i = 0; i < BASKET_MAX_FILES; i++) {
        if (!header.files[i].is_deleted && 
            fgsls_compare_tags(&header.files[i].tag, file_tag) == 0) {
            file_entry = &header.files[i];
            break;
        }
    }
    
    if (!file_entry) {
        return FGSLS_ERROR_FILE_NOT_FOUND;
    }
    
    // Check buffer size
    if (*size < file_entry->file_size) {
        *size = file_entry->file_size;
        return FGSLS_ERROR_INVALID_PARAMETER;
    }
    
    // TODO: Read file data from basket at file_entry->data_offset
    // In real implementation, this would read from physical storage
    // For now, simulate reading data
    memset(buffer, 0, file_entry->file_size);
    
    // Verify file integrity
    fgsls_hash_t calculated_hash;
    fgsls_calculate_hash(buffer, file_entry->file_size, &calculated_hash);
    
    if (memcmp(&calculated_hash, &file_entry->file_hash, sizeof(fgsls_hash_t)) != 0) {
        FGSLS_DEBUG_PRINT("Hash mismatch detected for file in basket");
        return FGSLS_ERROR_HASH_MISMATCH;
    }
    
    // Update access statistics
    file_entry->access_time = fgsls_get_current_time();
    entry->access_frequency++;
    entry->last_access = file_entry->access_time;
    
    // Write back updated basket header
    _fgsls_write_basket_header(system, &header);
    
    *size = file_entry->file_size;
    
    // Update system statistics
    system->total_reads++;
    
    // Log journal entry
    sant_journal_entry_t journal_entry;
    memset(&journal_entry, 0, sizeof(journal_entry));
    journal_entry.sequence_number = system->total_reads;
    journal_entry.timestamp = fgsls_get_current_time();
    journal_entry.operation_type = JOURNAL_READ;
    fgsls_copy_tag(&journal_entry.target_tag, file_tag);
    journal_entry.shelf_id = header.shelf_id;
    journal_entry.data_size = file_entry->file_size;
    snprintf(journal_entry.description, sizeof(journal_entry.description),
             "Read file '%s' (%u bytes) from basket", file_entry->filename, file_entry->file_size);
    
    fgsls_write_journal_entry(system, JOURNAL_WAREHOUSING_ENGINE, &journal_entry, sizeof(journal_entry));
    
    FGSLS_DEBUG_PRINT("Read file '%s' (%u bytes) from basket on shelf %d", 
                      file_entry->filename, file_entry->file_size, header.shelf_id);
    FGSLS_TRACE_EXIT("fgsls_read_file_from_basket", FGSLS_SUCCESS);
    return FGSLS_SUCCESS;
}

/**
 * Delete a file from a Basket
 */
int fgsls_delete_file_from_basket(fgsls_system_t *system, const fgsls_tag_t *file_tag) {
    FGSLS_TRACE_ENTER("fgsls_delete_file_from_basket");
    
    if (!system || !file_tag) {
        return FGSLS_ERROR_INVALID_PARAMETER;
    }
    
    if (!system->is_mounted) {
        return FGSLS_ERROR_SYSTEM_NOT_MOUNTED;
    }
    
    // Find file location using Taver
    fgsls_taver_index_t *taver = &system->taver_index;
    fgsls_position_entry_t *entry = NULL;
    uint32_t entry_index = 0;
    
    for (uint32_t i = 0; i < taver->entry_count; i++) {
        if (fgsls_compare_tags(&taver->entries[i].tag, file_tag) == 0) {
            entry = &taver->entries[i];
            entry_index = i;
            break;
        }
    }
    
    if (!entry || entry->container_type != CONTAINER_BASKET_FILE) {
        return FGSLS_ERROR_FILE_NOT_FOUND;
    }
    
    // Find basket tag
    fgsls_tag_t basket_tag;
    bool found_basket = false;
    
    for (uint32_t i = 0; i < taver->entry_count; i++) {
        if (taver->entries[i].container_type == CONTAINER_BASKET &&
            taver->entries[i].shelf_id == entry->shelf_id &&
            taver->entries[i].physical_offset == entry->physical_offset) {
            fgsls_copy_tag(&basket_tag, &taver->entries[i].tag);
            found_basket = true;
            break;
        }
    }
    
    if (!found_basket) {
        return FGSLS_ERROR_CORRUPTED_DATA;
    }
    
    // Read basket header
    fgsls_basket_header_t header;
    int result = _fgsls_read_basket_header(system, &basket_tag, &header);
    if (result != FGSLS_SUCCESS) {
        return result;
    }
    
    // Find file entry in basket
    fgsls_basket_file_entry_t *file_entry = NULL;
    for (uint32_t i = 0; i < BASKET_MAX_FILES; i++) {
        if (!header.files[i].is_deleted && 
            fgsls_compare_tags(&header.files[i].tag, file_tag) == 0) {
            file_entry = &header.files[i];
            break;
        }
    }
    
    if (!file_entry) {
        return FGSLS_ERROR_FILE_NOT_FOUND;
    }
    
    // Create garbage item for ZHT
    fgsls_garbage_item_t garbage_item;
    memset(&garbage_item, 0, sizeof(garbage_item));
    fgsls_copy_tag(&garbage_item.tag, file_tag);
    garbage_item.garbage_type = GARBAGE_ORPHANED_BASKET_FILE;
    garbage_item.shelf_id = header.shelf_id;
    garbage_item.size = file_entry->file_size;
    garbage_item.deletion_time = fgsls_get_current_time();
    garbage_item.quarantine_time = garbage_item.deletion_time;
    garbage_item.is_recoverable = true;
    memcpy(&garbage_item.data_hash, &file_entry->file_hash, sizeof(fgsls_hash_t));
    snprintf(garbage_item.description, sizeof(garbage_item.description),
             "Deleted file '%s' from basket", file_entry->filename);
    
    // Add to quarantine zone
    fgsls_quarantine_zone_t *quarantine = &system->zht_config.quarantine;
    if (quarantine->current_items < quarantine->max_items) {
        quarantine->items[quarantine->current_items] = garbage_item;
        quarantine->current_items++;
        quarantine->total_size += file_entry->file_size;
    }
    
    // Mark file as deleted (soft delete)
    file_entry->is_deleted = true;
    
    // Update basket statistics
    header.file_count--;
    header.deleted_count++;
    header.used_space -= file_entry->file_size;
    header.free_space += file_entry->file_size;
    
    // Recalculate basket hash
    fgsls_basket_header_t temp_header = header;
    memset(&temp_header.basket_hash, 0, sizeof(temp_header.basket_hash));
    fgsls_calculate_hash(&temp_header, sizeof(temp_header), &header.basket_hash);
    
    // Write updated basket header
    result = _fgsls_write_basket_header(system, &header);
    if (result != FGSLS_SUCCESS) {
        return result;
    }
    
    // Remove from Taver index
    memmove(&taver->entries[entry_index], &taver->entries[entry_index + 1],
            (taver->entry_count - entry_index - 1) * sizeof(fgsls_position_entry_t));
    taver->entry_count--;
    
    // Log journal entry
    sant_journal_entry_t journal_entry;
    memset(&journal_entry, 0, sizeof(journal_entry));
    journal_entry.sequence_number = system->total_writes++;
    journal_entry.timestamp = fgsls_get_current_time();
    journal_entry.operation_type = JOURNAL_DELETE;
    fgsls_copy_tag(&journal_entry.target_tag, file_tag);
    journal_entry.shelf_id = header.shelf_id;
    journal_entry.data_size = garbage_item.size;
    snprintf(journal_entry.description, sizeof(journal_entry.description),
             "Deleted file '%s' from basket", garbage_item.description + 13); // Skip "Deleted file '"
    
    fgsls_write_journal_entry(system, JOURNAL_WAREHOUSING_ENGINE, &journal_entry, sizeof(journal_entry));
    
    FGSLS_DEBUG_PRINT("Deleted file from basket on shelf %d", header.shelf_id);
    FGSLS_TRACE_EXIT("fgsls_delete_file_from_basket", FGSLS_SUCCESS);
    return FGSLS_SUCCESS;
}

/* ========================================================================
 * INTERNAL HELPER FUNCTIONS
 * ========================================================================*/

/**
 * Allocate physical space for a basket
 */
static int _fgsls_allocate_basket_space(fgsls_system_t *system, uint16_t shelf_id, 
                                       uint32_t basket_size, uint64_t *physical_offset) {
    // Simplified allocation - in real implementation, this would manage
    // free space bitmaps and find suitable locations
    fgsls_shelf_header_t *shelf = &system->shelves[shelf_id];
    
    // Calculate offset based on current usage
    *physical_offset = shelf->physical_start + shelf->config.used_size;
    
    return FGSLS_SUCCESS;
}

/**
 * Write basket header to storage
 */
static int _fgsls_write_basket_header(fgsls_system_t *system, const fgsls_basket_header_t *header) {
    // TODO: Implement actual disk I/O
    // This would write the header to the appropriate location on disk
    
    // In real implementation, write to disk at header->physical_offset
    
    return FGSLS_SUCCESS;
}

/**
 * Read basket header from storage
 */
static int _fgsls_read_basket_header(fgsls_system_t *system, const fgsls_tag_t *tag, 
                                    fgsls_basket_header_t *header) {
    // Find basket location using Taver
    fgsls_taver_index_t *taver = &system->taver_index;
    fgsls_position_entry_t *entry = NULL;
    
    for (uint32_t i = 0; i < taver->entry_count; i++) {
        if (fgsls_compare_tags(&taver->entries[i].tag, tag) == 0 &&
            taver->entries[i].container_type == CONTAINER_BASKET) {
            entry = &taver->entries[i];
            break;
        }
    }
    
    if (!entry) {
        return FGSLS_ERROR_FILE_NOT_FOUND;
    }
    
    // TODO: Read header from disk at entry->physical_offset
    // For now, create a dummy header
    memset(header, 0, sizeof(*header));
    fgsls_copy_tag(&header->tag, tag);
    header->shelf_id = entry->shelf_id;
    header->physical_offset = entry->physical_offset;
    header->basket_size = BASKET_DEFAULT_SIZE;
    
    return FGSLS_SUCCESS;
}

/**
 * Find a free file slot in basket
 */
static int _fgsls_find_free_file_slot(const fgsls_basket_header_t *header, uint32_t *slot_index) {
    for (uint32_t i = 0; i < BASKET_MAX_FILES; i++) {
        if (header->files[i].is_deleted) {
            *slot_index = i;
            return FGSLS_SUCCESS;
        }
    }
    
    return FGSLS_ERROR_BASKET_FULL;
}

/**
 * Compact basket by removing deleted files and defragmenting data
 */
static int _fgsls_compact_basket(fgsls_system_t *system, fgsls_basket_header_t *header) {
    FGSLS_DEBUG_PRINT("Compacting basket with %d deleted files", header->deleted_count);
    
    if (header->deleted_count == 0) {
        return FGSLS_SUCCESS; // Nothing to compact
    }
    
    // TODO: Implement actual compaction logic
    // This would involve:
    // 1. Reading all non-deleted file data
    // 2. Reorganizing data to eliminate gaps
    // 3. Updating data_offset for all files
    // 4. Writing compacted data back to disk
    
    // For now, simulate compaction by updating statistics
    uint64_t reclaimed_space = 0;
    
    for (uint32_t i = 0; i < BASKET_MAX_FILES; i++) {
        if (header->files[i].is_deleted && header->files[i].file_size > 0) {
            reclaimed_space += header->files[i].file_size;
            memset(&header->files[i], 0, sizeof(fgsls_basket_file_entry_t));
            header->files[i].is_deleted = true;
        }
    }
    
    header->free_space += reclaimed_space;
    header->used_space -= reclaimed_space;
    header->deleted_count = 0;
    header->last_compaction = fgsls_get_current_time();
    header->compaction_count++;
    
    FGSLS_DEBUG_PRINT("Basket compaction reclaimed %llu bytes", 
                      (unsigned long long)reclaimed_space);
    
    return FGSLS_SUCCESS;
}

/**
 * Update Taver position index for basket files
 */
static int _fgsls_update_basket_position(fgsls_system_t *system, const fgsls_tag_t *basket_tag,
                                        const fgsls_tag_t *file_tag, uint16_t shelf_id, 
                                        uint64_t physical_offset, uint32_t internal_offset) {
    fgsls_taver_index_t *taver = &system->taver_index;
    
    if (taver->entry_count >= taver->max_entries) {
        return FGSLS_ERROR_OUT_OF_MEMORY;
    }
    
    fgsls_position_entry_t *entry = &taver->entries[taver->entry_count];
    memset(entry, 0, sizeof(*entry));
    
    fgsls_copy_tag(&entry->tag, file_tag);
    entry->shelf_id = shelf_id;
    
    if (fgsls_compare_tags(basket_tag, file_tag) == 0) {
        // This is the basket itself
        entry->container_type = CONTAINER_BASKET;
        entry->internal_offset = 0;
        entry->size = BASKET_DEFAULT_SIZE;
    } else {
        // This is a file within the basket
        entry->container_type = CONTAINER_BASKET_FILE;
        entry->internal_offset = internal_offset;
        entry->size = 0; // Will be updated when file is actually written
    }
    
    entry->physical_offset = physical_offset;
    entry->last_access = fgsls_get_current_time();
    entry->access_frequency = 0;
    entry->is_fragmented = false;
    
    taver->entry_count++;
    taver->last_update = fgsls_get_current_time();
    
    return FGSLS_SUCCESS;
}