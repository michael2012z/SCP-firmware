/*
 * Arm SCP/MCP Software
 * Copyright (c) 2020-2021, Arm Limited and Contributors. All rights reserved.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef MOD_CMN700_H
#define MOD_CMN700_H

#include <fwk_id.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*!
 * \addtogroup GroupModules Modules
 * @{
 */

/*!
 * \defgroup GroupModuleCMN700 CMN700
 *
 * \brief Arm Coherent Mesh Network (CMN) 700 module
 *
 * \details This module adds support for the CMN700 interconnect
 * @{
 */

/*!
 * \brief Memory region configuration type
 */
enum mod_cmn700_mem_region_type {
    /*! Input/Output region (serviced by dedicated HN-I and HN-D nodes) */
    MOD_CMN700_MEM_REGION_TYPE_IO,

    /*!
     * Region backed by the system cache (serviced by all HN-F nodes in the
     * system)
     */
    MOD_CMN700_MEM_REGION_TYPE_SYSCACHE,

    /*!
     * Sub region of the system cache for non-hashed access (serviced by
     * dedicated SN-F nodes).
     */
    MOD_CMN700_REGION_TYPE_SYSCACHE_SUB,
};

/*!
 * \brief Coordinate (x, y, port no) of a node in the mesh
 */
struct node_pos {
    /*! x position of the node in the mesh */
    unsigned int pos_x;

    /*! y position of the node in the mesh */
    unsigned int pos_y;

    /*! port position of the node in the xp */
    unsigned int port_num;
};

/*!
 * \brief Memory region map descriptor
 */
struct mod_cmn700_mem_region_map {
    /*! Base address */
    uint64_t base;

    /*! Region size in bytes */
    uint64_t size;

    /*! Region configuration type */
    enum mod_cmn700_mem_region_type type;

    /*!
     * \brief Target node identifier
     *
     * \note Not used for \ref
     * mod_cmn700_mem_region_type.MOD_CMN700_REGION_TYPE_SYSCACHE_SUB
     * memory regions as it uses the pool of HN-F nodes available in the
     * system
     */
    unsigned int node_id;

    /*!
     * \brief HN-F start and end positions of a SCG/HTG
     *
     * \details Each SCG/HTG covers an address range and this address range can
     * be made to target a group of HN-Fs. These group of HN-Fs are typically
     * bound by an arbitrary rectangle/square in the mesh. To aid automatic
     * programming of the HN-Fs in SCG/HTG along with the discovery process,
     * each SCG/HTG takes hnf_pos_start and hnf_pos_end. HN-F nodes which are
     * bounded by this range will be assigned to the respective SCG/HTG. This
     * eliminates the process of manually looking at the mesh and assigning the
     * HN-F node ids to a SCG/HTG.
     *
     *                                        hnf_pos_end
     *                                             xx
     *                                            xx
     *                                           xx
     *                    ┌─────────────────────xx
     *                    │                     │
     *                    │                     │
     *                    │                     │
     *                    │                     │
     *                    │    nth- SCG/HTG     │
     *                    │                     │
     *                    │                     │
     *                    │                     │
     *                    │                     │
     *                   xx─────────────────────┘
     *                  xx
     *                 xx
     *                xx
     *         hnf_pos_start
     */

    /*!
     * \brief HN-F's bottom left node position
     *
     * \details \ref hnf_pos_start is the HN-F's bottom left node position in
     * the rectangle covering the HN-Fs for a SCG/HTG
     *
     * \note To be used only with \ref
     * mod_cmn700_mem_region_type.MOD_CMN700_MEM_REGION_TYPE_SYSCACHE memory
     * regions.
     */
    struct node_pos hnf_pos_start;

    /*!
     * \brief HN-F's top right node position
     *
     * \details \ref hnf_pos_start is the HN-F's bottom left node position in
     * the rectangle covering the HN-Fs for a SCG/HTG
     *
     * \note To be used only with \ref
     * mod_cmn700_mem_region_type.MOD_CMN700_MEM_REGION_TYPE_SYSCACHE memory
     * regions.
     */
    struct node_pos hnf_pos_end;
};

/*!
 * \brief HN-F to SN-F memory striping modes
 */
enum mod_cmn700_hnf_to_snf_mem_strip_mode {
    MOD_CMN700_1_SN_MODE,
    MOD_CMN700_3_SN_MODE,
    MOD_CMN700_6_SN_MODE,
    MOD_CMN700_5_SN_MODE,
};

/*!
 * \brief Hierarchical hashing configuration
 */
struct mod_cmn700_hierarchical_hashing {
    /*!
     * \brief Number of HN-Fs per cluster.
     *
     * \note The value should not account for \ref
     * mod_cmn700_config.hnf_cal_mode
     */
    unsigned int hnf_cluster_count;

    /*!
     * \brief HN-F to SN-F hashing mode.
     */
    enum mod_cmn700_hnf_to_snf_mem_strip_mode sn_mode;

    /*!
     * \brief Top PA address bit 0 to use for striping
     *
     * \note top_address_bit0 should match with the value in HN-F to SN-F strip
     * setting
     */
    unsigned int top_address_bit0;

    /*!
     * \brief Top PA address bit 1 to use for striping
     *
     * \note top_address_bit1 should match with the value in HN-F to SN-F strip
     * setting
     */
    unsigned int top_address_bit1;

    /*!
     * \brief Top PA address bit 2 to use for striping
     *
     * \note top_address_bit2 should match with the value in HN-F to SN-F strip
     * setting
     */
    unsigned int top_address_bit2;
};

/*!
 * \brief CMN700 configuration data
 */
struct mod_cmn700_config {
    /*! Peripheral base address. */
    uintptr_t base;

    /*! Size along x-axis of the interconnect mesh */
    unsigned int mesh_size_x;

    /*! Size along y-axis of the interconnect mesh */
    unsigned int mesh_size_y;

    /*! Default HN-D node identifier containing the global configuration */
    unsigned int hnd_node_id;

    /*!
     * \brief Table of SN-Fs used as targets for the HN-F nodes
     *
     * \details Each entry of this table corresponds to a HN-F node in the
     *      system. The HN-F's logical identifiers are used as indices in this
     *      table
     */
    const unsigned int *snf_table;

    /*! Number of entries in the \ref snf_table */
    size_t snf_count;

    /*! Hierarchical hashing support */
    bool hierarchical_hashing_enable;

    /*! Hierarchical hashing configuration */
    struct mod_cmn700_hierarchical_hashing hierarchical_hashing_config;

    /*! Table of region memory map entries */
    const struct mod_cmn700_mem_region_map *mmap_table;

    /*! Number of entries in the \ref mmap_table */
    size_t mmap_count;

    /*! Address space size of the chip */
    uint64_t chip_addr_space;

    /*! Identifier of the clock that this device depends on */
    fwk_id_t clock_id;

    /*!
     * \brief HN-F with CAL support flag
     * \details When set to true, enables HN-F with CAL support. This flag will
     * be used only if HN-F is found to be connected to CAL (When connected to
     * a CAL port, node id of HN-F will be a odd number).
     */
    bool hnf_cal_mode;
};

/*!
 * @}
 */

/*!
 * @}
 */

#endif /* MOD_CMN700_H */
