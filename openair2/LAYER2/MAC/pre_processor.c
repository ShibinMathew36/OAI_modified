/*
 * Licensed to the OpenAirInterface (OAI) Software Alliance under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.
 * The OpenAirInterface Software Alliance licenses this file to You under
 * the OAI Public License, Version 1.1  (the "License"); you may not use this file
 * except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.openairinterface.org/?page_id=698
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *-------------------------------------------------------------------------------
 * For more information about the OpenAirInterface (OAI) Software Alliance:
 *      contact@openairinterface.org
 */

/*! \file pre_processor.c
 * \brief eNB scheduler preprocessing fuction prior to scheduling
 * \author Navid Nikaein and Ankit Bhamri
 * \date 2013 - 2014
 * \email navid.nikaein@eurecom.fr
 * \version 1.0
 * @ingroup _mac

 */

#define _GNU_SOURCE
#include <stdlib.h>

#include "assertions.h"
#include "PHY/defs.h"
#include "PHY/extern.h"

#include "SCHED/defs.h"
#include "SCHED/extern.h"

#include "LAYER2/MAC/defs.h"
#include "LAYER2/MAC/proto.h"
#include "LAYER2/MAC/extern.h"
#include "UTIL/LOG/log.h"
#include "UTIL/LOG/vcd_signal_dumper.h"
#include "UTIL/OPT/opt.h"
#include "OCG.h"
#include "OCG_extern.h"
#include "RRC/LITE/extern.h"
#include "RRC/L2_INTERFACE/openair_rrc_L2_interface.h"
#include "rlc.h"



#define DEBUG_eNB_SCHEDULER 1
#define DEBUG_HEADER_PARSING 1
//#define DEBUG_PACKET_TRACE 1

//shibin- storing the average throughput per ue across sessions
uint16_t local_rnti[5] = {0, 0, 0, 0, 0};
//float avg_rate[5] = {-1.0, -1.0, -1.0, -1.0, -1.0};
UE_AVG_INFO ue_avg_info[5];
int total_ue_encountered = 0;

//#define ICIC 0

/*
  #ifndef USER_MODE
  #define msg debug_msg
  #endif
*/

/* this function checks that get_eNB_UE_stats returns
 * a non-NULL pointer for all the active CCs of an UE
 */
int phy_stats_exist(module_id_t Mod_id, int rnti)
{
  int CC_id;
  int i;
  int UE_id          = find_UE_id(Mod_id, rnti);
  UE_list_t *UE_list = &eNB_mac_inst[Mod_id].UE_list;
  if (UE_id == -1) {
    LOG_W(MAC, "[eNB %d] UE %x not found, should be there (in phy_stats_exist)\n",
	  Mod_id, rnti);
    return 0;
  }
  if (UE_list->numactiveCCs[UE_id] == 0) {
    LOG_W(MAC, "[eNB %d] UE %x has no active CC (in phy_stats_exist)\n",
	  Mod_id, rnti);
    return 0;
  }
  for (i = 0; i < UE_list->numactiveCCs[UE_id]; i++) {
    CC_id = UE_list->ordered_CCids[i][UE_id];
    if (mac_xface->get_eNB_UE_stats(Mod_id, CC_id, rnti) == NULL)
      return 0;
  }
  return 1;
}

// This function stores the downlink buffer for all the logical channels
void store_dlsch_buffer (module_id_t Mod_id,
                         frame_t     frameP,
                         sub_frame_t subframeP)
{

  int                   UE_id,i;
  rnti_t                rnti;
  mac_rlc_status_resp_t rlc_status;
  UE_list_t             *UE_list = &eNB_mac_inst[Mod_id].UE_list;
  UE_TEMPLATE           *UE_template;

  for (UE_id = 0; UE_id < NUMBER_OF_UE_MAX; UE_id++) {
    if (UE_list->active[UE_id] != TRUE) continue;

    UE_template = &UE_list->UE_template[UE_PCCID(Mod_id,UE_id)][UE_id];

    // clear logical channel interface variables
    UE_template->dl_buffer_total = 0;
    UE_template->dl_pdus_total = 0;

    for(i=0; i< MAX_NUM_LCID; i++) {
      UE_template->dl_buffer_info[i]=0;
      UE_template->dl_pdus_in_buffer[i]=0;
      UE_template->dl_buffer_head_sdu_creation_time[i]=0;
      UE_template->dl_buffer_head_sdu_remaining_size_to_send[i]=0;
    }

    rnti = UE_RNTI(Mod_id,UE_id);

    for(i=0; i< MAX_NUM_LCID; i++) { // loop over all the logical channels

      rlc_status = mac_rlc_status_ind(Mod_id,rnti, Mod_id,frameP,subframeP,ENB_FLAG_YES,MBMS_FLAG_NO,i,0 );
      UE_template->dl_buffer_info[i] = rlc_status.bytes_in_buffer; //storing the dlsch buffer for each logical channel
      UE_template->dl_pdus_in_buffer[i] = rlc_status.pdus_in_buffer;
      UE_template->dl_buffer_head_sdu_creation_time[i] = rlc_status.head_sdu_creation_time ;
      UE_template->dl_buffer_head_sdu_creation_time_max = cmax(UE_template->dl_buffer_head_sdu_creation_time_max,
          rlc_status.head_sdu_creation_time );
      UE_template->dl_buffer_head_sdu_remaining_size_to_send[i] = rlc_status.head_sdu_remaining_size_to_send;
      UE_template->dl_buffer_head_sdu_is_segmented[i] = rlc_status.head_sdu_is_segmented;
      UE_template->dl_buffer_total += UE_template->dl_buffer_info[i];//storing the total dlsch buffer
      UE_template->dl_pdus_total   += UE_template->dl_pdus_in_buffer[i];

#ifdef DEBUG_eNB_SCHEDULER

      /* note for dl_buffer_head_sdu_remaining_size_to_send[i] :
       * 0 if head SDU has not been segmented (yet), else remaining size not already segmented and sent
       */
      if (UE_template->dl_buffer_info[i]>0)
        LOG_D(MAC,
              "[eNB %d] Frame %d Subframe %d : RLC status for UE %d in LCID%d: total of %d pdus and size %d, head sdu queuing time %d, remaining size %d, is segmeneted %d \n",
              Mod_id, frameP, subframeP, UE_id,
              i, UE_template->dl_pdus_in_buffer[i],UE_template->dl_buffer_info[i],
              UE_template->dl_buffer_head_sdu_creation_time[i],
              UE_template->dl_buffer_head_sdu_remaining_size_to_send[i],
              UE_template->dl_buffer_head_sdu_is_segmented[i]
             );

#endif

    }

    //#ifdef DEBUG_eNB_SCHEDULER
    if ( UE_template->dl_buffer_total>0)
      LOG_D(MAC,"[eNB %d] Frame %d Subframe %d : RLC status for UE %d : total DL buffer size %d and total number of pdu %d \n",
            Mod_id, frameP, subframeP, UE_id,
            UE_template->dl_buffer_total,
            UE_template->dl_pdus_total
           );

    //#endif
  }
}


// This function returns the estimated number of RBs required by each UE for downlink scheduling
void assign_rbs_required (module_id_t Mod_id,
                          frame_t     frameP,
                          sub_frame_t subframe,
                          uint16_t    nb_rbs_required[MAX_NUM_CCs][NUMBER_OF_UE_MAX],
                          int         min_rb_unit[MAX_NUM_CCs],
                          float ach_rate[MAX_NUM_CCs][NUMBER_OF_UE_MAX])
{

  LOG_D(MAC,"Shibin  assign_rbs_required\n");
  rnti_t           rnti;
  uint16_t         TBS = 0;
  LTE_eNB_UE_stats *eNB_UE_stats[MAX_NUM_CCs];
  int              UE_id,n,i,j,CC_id,pCCid,tmp;
  UE_list_t        *UE_list = &eNB_mac_inst[Mod_id].UE_list;
  //  UE_TEMPLATE           *UE_template;
  LTE_DL_FRAME_PARMS   *frame_parms[MAX_NUM_CCs];

  // clear rb allocations across all CC_ids
  for (UE_id = 0; UE_id < NUMBER_OF_UE_MAX; UE_id++) {
    if (UE_list->active[UE_id] != TRUE) continue;

    pCCid = UE_PCCID(Mod_id,UE_id);
    rnti = UE_list->UE_template[pCCid][UE_id].rnti;

    /* skip UE not present in PHY (for any of its active CCs) */
    if (!phy_stats_exist(Mod_id, rnti))
      continue;

    //update CQI information across component carriers
    for (n=0; n<UE_list->numactiveCCs[UE_id]; n++) {

      CC_id = UE_list->ordered_CCids[n][UE_id];
      frame_parms[CC_id] = mac_xface->get_lte_frame_parms(Mod_id,CC_id);
      eNB_UE_stats[CC_id] = mac_xface->get_eNB_UE_stats(Mod_id,CC_id,rnti);
      /*
      DevCheck(((eNB_UE_stats[CC_id]->DL_cqi[0] < MIN_CQI_VALUE) || (eNB_UE_stats[CC_id]->DL_cqi[0] > MAX_CQI_VALUE)),
      eNB_UE_stats[CC_id]->DL_cqi[0], MIN_CQI_VALUE, MAX_CQI_VALUE);
      */
      eNB_UE_stats[CC_id]->dlsch_mcs1=cqi_to_mcs[eNB_UE_stats[CC_id]->DL_cqi[0]]; // shibin - each carrier gets allocated the respective mcs value

      //eNB_UE_stats[CC_id]->dlsch_mcs1 = eNB_UE_stats[CC_id]->dlsch_mcs1; cmin(eNB_UE_stats[CC_id]->dlsch_mcs1,openair_daq_vars.target_ue_dl_mcs);

    }
    // shibin - modify the 2D array to store mcs as
    // provide the list of CCs sorted according to MCS
    for (i=0; i<UE_list->numactiveCCs[UE_id]; i++) {
      for (j=i+1; j<UE_list->numactiveCCs[UE_id]; j++) {
        DevAssert( j < MAX_NUM_CCs );

        if (eNB_UE_stats[UE_list->ordered_CCids[i][UE_id]]->dlsch_mcs1 >
            eNB_UE_stats[UE_list->ordered_CCids[j][UE_id]]->dlsch_mcs1) {
          tmp = UE_list->ordered_CCids[i][UE_id];
          UE_list->ordered_CCids[i][UE_id] = UE_list->ordered_CCids[j][UE_id];
          UE_list->ordered_CCids[j][UE_id] = tmp;
        }
      }
    }

    /*
    if ((mac_get_rrc_status(Mod_id,1,UE_id) < RRC_RECONFIGURED)){  // If we still don't have a default radio bearer
      nb_rbs_required[pCCid][UE_id] = PHY_vars_eNB_g[Mod_id][pCCid]->frame_parms.N_RB_DL;
      continue;
    }
    */
    /* NN --> RK
     * check the index of UE_template"
     */
    //    if (UE_list->UE_template[UE_id]->dl_buffer_total> 0) {
    if (UE_list->UE_template[pCCid][UE_id].dl_buffer_total> 0) {
      LOG_D(MAC,"[preprocessor] assign RB for UE %d\n",UE_id);

      for (i=0; i<UE_list->numactiveCCs[UE_id]; i++) {
        CC_id = UE_list->ordered_CCids[i][UE_id];
        frame_parms[CC_id] = mac_xface->get_lte_frame_parms(Mod_id,CC_id);
        eNB_UE_stats[CC_id] = mac_xface->get_eNB_UE_stats(Mod_id,CC_id,rnti);

        if (eNB_UE_stats[CC_id]->dlsch_mcs1==0) {
          nb_rbs_required[CC_id][UE_id] = 4;  // don't let the TBS get too small
        } else {
          nb_rbs_required[CC_id][UE_id] = min_rb_unit[CC_id];
        }

        TBS = mac_xface->get_TBS_DL(eNB_UE_stats[CC_id]->dlsch_mcs1,nb_rbs_required[CC_id][UE_id]);

        LOG_I(MAC,"[preprocessor] start RB assignement for UE %d CC_id %d dl buffer %d (RB unit %d, MCS %d, TBS %d) \n",
              UE_id, CC_id, UE_list->UE_template[pCCid][UE_id].dl_buffer_total,
              nb_rbs_required[CC_id][UE_id],eNB_UE_stats[CC_id]->dlsch_mcs1,TBS);

        /* calculating required number of RBs for each UE */
        while (TBS < UE_list->UE_template[pCCid][UE_id].dl_buffer_total)  {
          nb_rbs_required[CC_id][UE_id] += min_rb_unit[CC_id];

          if (nb_rbs_required[CC_id][UE_id] > frame_parms[CC_id]->N_RB_DL) {
            TBS = mac_xface->get_TBS_DL(eNB_UE_stats[CC_id]->dlsch_mcs1,frame_parms[CC_id]->N_RB_DL);
            nb_rbs_required[CC_id][UE_id] = frame_parms[CC_id]->N_RB_DL;  // shibin limiting the RB to the max available per carrier
            break;
          }

          TBS = mac_xface->get_TBS_DL(eNB_UE_stats[CC_id]->dlsch_mcs1,nb_rbs_required[CC_id][UE_id]);
        } // end of while
          // shibin - use the TBS value calculated here for priority index calculation
        float old_rate = 1.0;   // shibin check if this value is to be initialized to something else
        for (int z = 0; z<total_ue_encountered; z++) if (ue_avg_info[z].rnti == rnti) old_rate = ue_avg_info[z].avg_rate;
        LOG_I(MAC,"Shibin calculated avg rate for ue %d is %f \n", UE_id, old_rate);

        ach_rate[CC_id][UE_id] = ((float)TBS/.001)/old_rate;  // is the right way as old rate involves all cc and here numerator is just one cc
        //LOG_I(MAC,"Shibin [eNB %d] Frame %d: UE %d on CC %d: RB unit %d,  nb_required RB %d (TBS %d, mcs %d)\n",
              //Mod_id, frameP,UE_id, CC_id,  min_rb_unit[CC_id], nb_rbs_required[CC_id][UE_id], TBS, eNB_UE_stats[CC_id]->dlsch_mcs1);
      }
    }
  }
}


// This function scans all CC_ids for a particular UE to find the maximum round index of its HARQ processes

int maxround(module_id_t Mod_id,uint16_t rnti,int frame,sub_frame_t subframe,uint8_t ul_flag )
{

  uint8_t round,round_max=0,UE_id;
  int CC_id;
  UE_list_t *UE_list = &eNB_mac_inst[Mod_id].UE_list;

  for (CC_id=0; CC_id<MAX_NUM_CCs; CC_id++) {

    UE_id = find_UE_id(Mod_id,rnti);
    round    = UE_list->UE_sched_ctrl[UE_id].round[CC_id];
    if (round > round_max) {
      round_max = round;
    }
  }

  return round_max;
}

// This function scans all CC_ids for a particular UE to find the maximum DL CQI
// it returns -1 if the UE is not found in PHY layer (get_eNB_UE_stats gives NULL)
int maxcqi(module_id_t Mod_id,int32_t UE_id)
{

  LTE_eNB_UE_stats *eNB_UE_stats = NULL;
  UE_list_t *UE_list = &eNB_mac_inst[Mod_id].UE_list;
  int CC_id,n;
  int CQI = 0;

  for (n=0; n<UE_list->numactiveCCs[UE_id]; n++) {
    CC_id = UE_list->ordered_CCids[n][UE_id];
    eNB_UE_stats = mac_xface->get_eNB_UE_stats(Mod_id,CC_id,UE_RNTI(Mod_id,UE_id));

    if (eNB_UE_stats==NULL) {
      /* the UE may have been removed in the PHY layer, don't exit */
      //mac_xface->macphy_exit("maxcqi: could not get eNB_UE_stats\n");
      return -1;
    }

    if (eNB_UE_stats->DL_cqi[0] > CQI) {
      CQI = eNB_UE_stats->DL_cqi[0];
    }
  }

  return(CQI);
}

// This function assigns pre-available RBS to each UE in specified sub-bands before scheduling is done
void dlsch_scheduler_pre_processor (module_id_t   Mod_id,
                                    frame_t       frameP,
                                    sub_frame_t   subframeP,
                                    int           N_RBG[MAX_NUM_CCs],
                                    int           *mbsfn_flag)
{
  LOG_D(MAC,"Shibin inside dlsch_scheduler_pre_processor \n");
  unsigned char rballoc_sub[MAX_NUM_CCs][N_RBG_MAX],harq_pid=0,round=0,total_ue_count;
  unsigned char MIMO_mode_indicator[MAX_NUM_CCs][N_RBG_MAX];
  int                     UE_id, i;
  uint16_t                ii,j;
  uint16_t                nb_rbs_required[MAX_NUM_CCs][NUMBER_OF_UE_MAX];
  uint16_t                nb_rbs_required_remaining[MAX_NUM_CCs][NUMBER_OF_UE_MAX];
  //uint16_t                nb_rbs_required_remaining_1[MAX_NUM_CCs][NUMBER_OF_UE_MAX];
  rnti_t             rnti;
  int                min_rb_unit[MAX_NUM_CCs];
  //uint16_t r1=0;
  uint8_t CC_id;
  UE_list_t *UE_list = &eNB_mac_inst[Mod_id].UE_list;
  LTE_DL_FRAME_PARMS   *frame_parms[MAX_NUM_CCs] = {0};

  // shibin this stores the achievable rate (TBS/tti) in the order of ue per cc
  float ach_rate[MAX_NUM_CCs][NUMBER_OF_UE_MAX];
  for(int m =0; m < MAX_NUM_CCs; m++)
      for(int n =0; n < NUMBER_OF_UE_MAX; n++)
          ach_rate[m][n] = -1.0;

  int transmission_mode = 0;
  UE_sched_ctrl *ue_sched_ctl;
  //  int rrc_status           = RRC_IDLE;

#ifdef TM5
  int harq_pid1=0,harq_pid2=0;
  int round1=0,round2=0;
  int UE_id2;
  uint16_t                i1,i2,i3;
  rnti_t             rnti1,rnti2;
  LTE_eNB_UE_stats  *eNB_UE_stats1 = NULL;
  LTE_eNB_UE_stats  *eNB_UE_stats2 = NULL;
  UE_sched_ctrl *ue_sched_ctl1,*ue_sched_ctl2;
#endif

  for (CC_id=0; CC_id<MAX_NUM_CCs; CC_id++) {

    if (mbsfn_flag[CC_id]>0)  // If this CC is allocated for MBSFN skip it here
      continue;

    frame_parms[CC_id] = mac_xface->get_lte_frame_parms(Mod_id,CC_id);


    min_rb_unit[CC_id]=get_min_rb_unit(Mod_id,CC_id);

    for (i = 0; i < NUMBER_OF_UE_MAX; i++) {
      if (UE_list->active[i] != TRUE) continue;
        LOG_D(MAC,"Shibin found active UE\n");
      UE_id = i;
      // Initialize scheduling information for all active UEs



      dlsch_scheduler_pre_processor_reset(Mod_id,
        UE_id,
        CC_id,
        frameP,
        subframeP,
        N_RBG[CC_id],
        nb_rbs_required,
        nb_rbs_required_remaining,
        rballoc_sub,
        MIMO_mode_indicator);

    }
  }


  // Store the DLSCH buffer for each logical channel
  store_dlsch_buffer (Mod_id,frameP,subframeP);
  LOG_D(MAC,"Shibin after store_dlsch_buffer\n");


    // Calculate the number of RBs required by each UE on the basis of logical channel's buffer
  assign_rbs_required (Mod_id,frameP,subframeP,nb_rbs_required,min_rb_unit, ach_rate);


  //LOG_I(MAC,"Shibin after assign_rbs_required\n");
  // Sorts the user on the basis of dlsch logical channel buffer and CQI
  //sort_UEs (Mod_id,frameP,subframeP);



  total_ue_count =0;

  // loop over all active UEs
  for (i=UE_list->head; i>=0; i=UE_list->next[i]) {
    LOG_D(MAC,"Shibin entering inside for loop \n");
    rnti = UE_RNTI(Mod_id,i);

    if(rnti == NOT_A_RNTI){
        LOG_D(MAC,"Shibin  NOT_A_RNTI\n");
        continue;
    }
    if (UE_list->UE_sched_ctrl[i].ul_out_of_sync == 1){
        LOG_D(MAC,"Shibin UR out of sync \n");
        continue;
    }
    if (!phy_stats_exist(Mod_id, rnti)){
        LOG_D(MAC,"Shibin phy_stats_ doesnt exist \n");
        continue;
    }
    UE_id = i;

    for (ii=0; ii<UE_num_active_CC(UE_list,UE_id); ii++) {
      LOG_D(MAC,"Shibin inside for loop \n");
      CC_id = UE_list->ordered_CCids[ii][UE_id];
      ue_sched_ctl = &UE_list->UE_sched_ctrl[UE_id];
      harq_pid = ue_sched_ctl->harq_pid[CC_id];
      round    = ue_sched_ctl->round[CC_id];

      // if there is no available harq_process, skip the UE
      if (UE_list->UE_sched_ctrl[UE_id].harq_pid[CC_id]<0)
        continue;

      //average_rbs_per_user[CC_id]=0;

      frame_parms[CC_id] = mac_xface->get_lte_frame_parms(Mod_id,CC_id);

      //      mac_xface->get_ue_active_harq_pid(Mod_id,CC_id,rnti,frameP,subframeP,&harq_pid,&round,0);

      if(round>0) {
        nb_rbs_required[CC_id][UE_id] = UE_list->UE_template[CC_id][UE_id].nb_rb[harq_pid];
      }

      //nb_rbs_required_remaining[UE_id] = nb_rbs_required[UE_id];
      if (nb_rbs_required[CC_id][UE_id] > 0) {
        total_ue_count = total_ue_count + 1;
          LOG_I(MAC,"Shibin inside nb_rbs_required[CC_id][UE_id] > 0\n");
      }
    }
  }
  int bugger = 0;
  if (total_ue_count != 0) bugger = 1;
  if (bugger) LOG_I(MAC,"Shibin total ue count = %d \n", total_ue_count);

  // shibin below code to get all active cc across all UE
  uint8_t valid_CCs[MAX_NUM_CCs];
  int temp = 0;
  if (total_ue_count > 0 ) {
    for (i = UE_list->head; i >= 0; i = UE_list->next[i]) {
      UE_id = i;

      for (ii = 0; ii < UE_num_active_CC(UE_list, UE_id); ii++) {
        if (bugger) LOG_I(MAC,"Shibin number of active cc existing = %d\n", UE_num_active_CC(UE_list, UE_id));
        CC_id = UE_list->ordered_CCids[ii][UE_id];
        ue_sched_ctl = &UE_list->UE_sched_ctrl[UE_id];
        harq_pid = ue_sched_ctl->harq_pid[CC_id];
        round = ue_sched_ctl->round[CC_id];

        rnti = UE_RNTI(Mod_id, UE_id);

        // LOG_D(MAC,"UE %d rnti 0x\n", UE_id, rnti );
        if (rnti == NOT_A_RNTI) {
            if (bugger) LOG_I(MAC, "Shibin caught invalid RNTI\n");
            continue;
        }
        if (UE_list->UE_sched_ctrl[UE_id].ul_out_of_sync == 1) {
            if (bugger) LOG_I(MAC,"Shibin UE out of synch\n");
            continue;
        }
        if (!phy_stats_exist(Mod_id, rnti)){
            if (bugger) LOG_I(MAC,"Shibin phy status not existing\n");
            continue;
        }
        int z = 0;
        for(; z < temp; z++) if (valid_CCs[z] == CC_id) break;
        if (z == temp){
            valid_CCs[temp] = CC_id;
            temp += 1;
        }
      }
    }
  }
  
  // shibin creating a local array struct type UE_TEMP_INFO to store allocation detail
  UE_TEMP_INFO local_rb_allocations[total_ue_count];
  int local_stored = 0; // to keep track of number of local objects created

  for (i =0; i < temp; i++){
    int index = 0;
    int UE_per_cc[5];
    float priority_index[5];
    for(int dummy = 0; dummy < 5; dummy++) UE_per_cc[dummy] = -1;
    for(int dummy = 0; dummy < 5; dummy++) priority_index[dummy] = -1.0;
    for (int z =0; z < NUMBER_OF_UE_MAX; z++){
      if (ach_rate[valid_CCs[i]][z] != -1.0){
        priority_index[index] = ach_rate[valid_CCs[i]][z];
        UE_per_cc[index] = z;
        index++;
      }
    }
    LOG_I(MAC,"Shibin total UE for cc %d is = %d \n", i, index);
    // shibin now sort the UE in each cc based on their priority value
    for(int a = 0; a<index; a++){
      for(int b = a + 1; b<index; b++){
        if (priority_index[a] < priority_index[b]){
          int val_ue = UE_per_cc[a];
          UE_per_cc[a] = UE_per_cc[b];
          UE_per_cc[b] = val_ue;
        }
      }
    }

    for(int a = 0; a<index; a++){
      UE_id = UE_per_cc[a];
      CC_id = valid_CCs[i];
      ue_sched_ctl = &UE_list->UE_sched_ctrl[UE_id];
      harq_pid = ue_sched_ctl->harq_pid[CC_id];
      round    = ue_sched_ctl->round[CC_id];

      rnti = UE_RNTI(Mod_id,UE_id);

      // LOG_D(MAC,"UE %d rnti 0x\n", UE_id, rnti );
      if(rnti == NOT_A_RNTI)
        continue;
      if (UE_list->UE_sched_ctrl[UE_id].ul_out_of_sync == 1)
        continue;
      if (!phy_stats_exist(Mod_id, rnti))
        continue;

      transmission_mode = mac_xface->get_transmission_mode(Mod_id,CC_id,rnti);

      // shibin - code to generate pointer to stored total rb allocations to that UE
      UE_TEMP_INFO *UE_to_edit;
      int z =0;
      for(; z < local_stored; z++)
          if (UE_id == local_rb_allocations[z].UE_id) UE_to_edit = &local_rb_allocations[z];

      if (z == local_stored){
          UE_TEMP_INFO temp_struct;
          temp_struct.UE_id = UE_id;
          temp_struct.total_tbs_rate = 0.0;
          local_rb_allocations[local_stored] = temp_struct;
          UE_to_edit = &local_rb_allocations[local_stored];
          local_stored += 1;
      }
      if (bugger)  LOG_I(MAC,"calling dlsch_scheduler_pre_processor_allocate .. \n ");
      //shibin - actual assignment has to take place here
      dlsch_scheduler_pre_processor_allocate (Mod_id,
                                              CC_id,
                                              N_RBG[CC_id],
                                              transmission_mode,
                                              min_rb_unit[CC_id],
                                              frame_parms[CC_id]->N_RB_DL,
                                              nb_rbs_required,
                                              //nb_rbs_required_remaining,
                                              rballoc_sub,
                                              MIMO_mode_indicator, UE_id, UE_to_edit);
    }

  }

  //shibin updating the current value to 0 for all stored UE details
  for (int x = 0; x<total_ue_encountered; x++) {
      ue_avg_info[x].current_tti = 0.0;
  }
  // shibin update the stored average rate based on the allocation in this TTI
  for (int z=0; z<local_stored; z++){
      int x = 0;
      for (; x<total_ue_encountered; x++){
          if (ue_avg_info[x].rnti == UE_RNTI(Mod_id,local_rb_allocations[z].UE_id)){
              ue_avg_info[x].current_tti = (1/99)*local_rb_allocations[z].total_tbs_rate;
          }
      }
      if (x == total_ue_encountered){
          UE_AVG_INFO temp_avg_info;
          temp_avg_info.rnti = UE_RNTI(Mod_id,local_rb_allocations[z].UE_id);
          ue_avg_info[x].current_tti = (1/99)*local_rb_allocations[z].total_tbs_rate;
          temp_avg_info.avg_rate = 0.0;
          ue_avg_info[x] = temp_avg_info;
          total_ue_encountered += 1;
      }
  }

  if (bugger) LOG_I(MAC,"Shibin  total stored UE is = %d ue encountered in this TTI = %d", total_ue_encountered, local_stored);
  // shibin - update the rate of UE not in the current TTI
  for (int x = 0; x<total_ue_encountered; x++) {
      ue_avg_info[x].avg_rate = (1 - 1/99)*ue_avg_info[x].avg_rate + ue_avg_info[x].current_tti;
      if (bugger) LOG_I(MAC,"Shibin  tfinal stored values UE ID = %d and avg rate = %f", ue_avg_info[x].rnti, ue_avg_info[x].avg_rate);
  }

#ifdef TM5

          // data chanel TM5: to be revisted
          if ((round == 0 )  &&
              (transmission_mode == 5)  &&
              (ue_sched_ctl->dl_pow_off[CC_id] != 1)) {

            for(j=0; j<N_RBG[CC_id]; j+=2) {

              if( (((j == (N_RBG[CC_id]-1))&& (rballoc_sub[CC_id][j] == 0) && (ue_sched_ctl->rballoc_sub_UE[CC_id][j] == 0))  ||
                   ((j < (N_RBG[CC_id]-1)) && (rballoc_sub[CC_id][j+1] == 0) && (ue_sched_ctl->rballoc_sub_UE[CC_id][j+1] == 0)) ) &&
                  (nb_rbs_required_remaining[CC_id][UE_id]>0)) {

                for (ii = UE_list->next[i+1]; ii >=0; ii=UE_list->next[ii]) {

                  UE_id2 = ii;
                  rnti2 = UE_RNTI(Mod_id,UE_id2);
		  ue_sched_ctl2 = &UE_list->UE_sched_ctrl[UE_id2];
		  harq_pid2 = ue_sched_ctl2->harq_pid[CC_id];
		  round2    = ue_sched_ctl2->round[CC_id];
                  if(rnti2 == NOT_A_RNTI)
                    continue;
		  if (UE_list->UE_sched_ctrl[UE_id2].ul_out_of_sync == 1)
		    continue;
                  if (!phy_stats_exist(Mod_idP, rnti2))
                    continue;

                  eNB_UE_stats2 = mac_xface->get_eNB_UE_stats(Mod_id,CC_id,rnti2);
                  //mac_xface->get_ue_active_harq_pid(Mod_id,CC_id,rnti2,frameP,subframeP,&harq_pid2,&round2,0);

                  if ((mac_eNB_get_rrc_status(Mod_id,rnti2) >= RRC_RECONFIGURED) &&
                      (round2==0) &&
                      (mac_xface->get_transmission_mode(Mod_id,CC_id,rnti2)==5) &&
                      (ue_sched_ctl->dl_pow_off[CC_id] != 1)) {

                    if( (((j == (N_RBG[CC_id]-1)) && (ue_sched_ctl->rballoc_sub_UE[CC_id][j] == 0)) ||
                         ((j < (N_RBG[CC_id]-1)) && (ue_sched_ctl->rballoc_sub_UE[CC_id][j+1] == 0))  ) &&
                        (nb_rbs_required_remaining[CC_id][UE_id2]>0)) {

                      if((((eNB_UE_stats2->DL_pmi_single^eNB_UE_stats1->DL_pmi_single)<<(14-j))&0xc000)== 0x4000) { //MU-MIMO only for 25 RBs configuration

                        rballoc_sub[CC_id][j] = 1;
                        ue_sched_ctl->rballoc_sub_UE[CC_id][j] = 1;
                        ue_sched_ctl2->rballoc_sub_UE[CC_id][j] = 1;
                        MIMO_mode_indicator[CC_id][j] = 0;

                        if (j< N_RBG[CC_id]-1) {
                          rballoc_sub[CC_id][j+1] = 1;
                          ue_sched_ctl->rballoc_sub_UE[CC_id][j+1] = 1;
                          ue_sched_ctl2->rballoc_sub_UE[CC_id][j+1] = 1;
                          MIMO_mode_indicator[CC_id][j+1] = 0;
                        }

                        ue_sched_ctl->dl_pow_off[CC_id] = 0;
                        ue_sched_ctl2->dl_pow_off[CC_id] = 0;


                        if ((j == N_RBG[CC_id]-1) &&
                            ((PHY_vars_eNB_g[Mod_id][CC_id]->frame_parms.N_RB_DL == 25) ||
                             (PHY_vars_eNB_g[Mod_id][CC_id]->frame_parms.N_RB_DL == 50))) {

                          nb_rbs_required_remaining[CC_id][UE_id] = nb_rbs_required_remaining[CC_id][UE_id] - min_rb_unit[CC_id]+1;
                          ue_sched_ctl->pre_nb_available_rbs[CC_id] = ue_sched_ctl->pre_nb_available_rbs[CC_id] + min_rb_unit[CC_id]-1;
                          nb_rbs_required_remaining[CC_id][UE_id2] = nb_rbs_required_remaining[CC_id][UE_id2] - min_rb_unit[CC_id]+1;
                          ue_sched_ctl2->pre_nb_available_rbs[CC_id] = ue_sched_ctl2->pre_nb_available_rbs[CC_id] + min_rb_unit[CC_id]-1;
                        } else {

			  nb_rbs_required_remaining[CC_id][UE_id] = nb_rbs_required_remaining[CC_id][UE_id] - 4;
                          ue_sched_ctl->pre_nb_available_rbs[CC_id] = ue_sched_ctl->pre_nb_available_rbs[CC_id] + 4;
                          nb_rbs_required_remaining[CC_id][UE_id2] = nb_rbs_required_remaining[CC_id][UE_id2] - 4;
                          ue_sched_ctl2->pre_nb_available_rbs[CC_id] = ue_sched_ctl2->pre_nb_available_rbs[CC_id] + 4;
                        }

                        break;
                      }
                    }
                  }
                }
              }
            }
          }

#endif
       // }
     // }
    //} // total_ue_count
  //}  end of for for r1 and r2

#ifdef TM5

  // This has to be revisited!!!!
  for (CC_id=0; CC_id<MAX_NUM_CCs; CC_id++) {
    i1=0;
    i2=0;
    i3=0;

    for (j=0; j<N_RBG[CC_id]; j++) {
      if(MIMO_mode_indicator[CC_id][j] == 2) {
        i1 = i1+1;
      } else if(MIMO_mode_indicator[CC_id][j] == 1) {
        i2 = i2+1;
      } else if(MIMO_mode_indicator[CC_id][j] == 0) {
        i3 = i3+1;
      }
    }

    if((i1 < N_RBG[CC_id]) && (i2>0) && (i3==0)) {
      PHY_vars_eNB_g[Mod_id][CC_id]->check_for_SUMIMO_transmissions = PHY_vars_eNB_g[Mod_id][CC_id]->check_for_SUMIMO_transmissions + 1;
    }

    if(i3 == N_RBG[CC_id] && i1==0 && i2==0) {
      PHY_vars_eNB_g[Mod_id][CC_id]->FULL_MUMIMO_transmissions = PHY_vars_eNB_g[Mod_id][CC_id]->FULL_MUMIMO_transmissions + 1;
    }

    if((i1 < N_RBG[CC_id]) && (i3 > 0)) {
      PHY_vars_eNB_g[Mod_id][CC_id]->check_for_MUMIMO_transmissions = PHY_vars_eNB_g[Mod_id][CC_id]->check_for_MUMIMO_transmissions + 1;
    }

    PHY_vars_eNB_g[Mod_id][CC_id]->check_for_total_transmissions = PHY_vars_eNB_g[Mod_id][CC_id]->check_for_total_transmissions + 1;

  }

#endif

  for(i=UE_list->head; i>=0; i=UE_list->next[i]) {
    UE_id = i;
    ue_sched_ctl = &UE_list->UE_sched_ctrl[UE_id];

    for (ii=0; ii<UE_num_active_CC(UE_list,UE_id); ii++) {
      CC_id = UE_list->ordered_CCids[ii][UE_id];
      //PHY_vars_eNB_g[Mod_id]->mu_mimo_mode[UE_id].dl_pow_off = dl_pow_off[UE_id];

      if (ue_sched_ctl->pre_nb_available_rbs[CC_id] > 0 ) {
        LOG_D(MAC,"******************DL Scheduling Information for UE%d ************************\n",UE_id);
        LOG_D(MAC,"dl power offset UE%d = %d \n",UE_id,ue_sched_ctl->dl_pow_off[CC_id]);
        LOG_D(MAC,"***********RB Alloc for every subband for UE%d ***********\n",UE_id);

        for(j=0; j<N_RBG[CC_id]; j++) {
          //PHY_vars_eNB_g[Mod_id]->mu_mimo_mode[UE_id].rballoc_sub[i] = rballoc_sub_UE[CC_id][UE_id][i];
          // LOG_D(MAC,"RB Alloc for UE%d and Subband%d = %d\n",UE_id,j,ue_sched_ctl->rballoc_sub_UE[CC_id][j]);
        }

        //PHY_vars_eNB_g[Mod_id]->mu_mimo_mode[UE_id].pre_nb_available_rbs = pre_nb_available_rbs[CC_id][UE_id];
        LOG_D(MAC,"Total RBs allocated for UE%d = %d\n",UE_id,ue_sched_ctl->pre_nb_available_rbs[CC_id]);
      }
    }
  }
}

#define SF0_LIMIT 1

void dlsch_scheduler_pre_processor_reset (int module_idP,
					  int UE_id,
					  uint8_t  CC_id,
					  int frameP,
					  int subframeP,
					  int N_RBG,
					  uint16_t nb_rbs_required[MAX_NUM_CCs][NUMBER_OF_UE_MAX],
					  uint16_t nb_rbs_required_remaining[MAX_NUM_CCs][NUMBER_OF_UE_MAX],
					  unsigned char rballoc_sub[MAX_NUM_CCs][N_RBG_MAX],
					  unsigned char MIMO_mode_indicator[MAX_NUM_CCs][N_RBG_MAX])

{
  int i,j;
  UE_list_t *UE_list=&eNB_mac_inst[module_idP].UE_list;
  UE_sched_ctrl *ue_sched_ctl = &UE_list->UE_sched_ctrl[UE_id];
  rnti_t rnti = UE_RNTI(module_idP,UE_id);
  uint8_t *vrb_map = eNB_mac_inst[module_idP].common_channels[CC_id].vrb_map;
  int RBGsize;
  int RBGsize_last;
#ifdef SF0_LIMIT
  int sf0_upper=-1,sf0_lower=-1;
#endif
  LTE_eNB_UE_stats *eNB_UE_stats = mac_xface->get_eNB_UE_stats(module_idP,CC_id,rnti);
  if (eNB_UE_stats == NULL) return;

  // initialize harq_pid and round

  if (eNB_UE_stats == NULL)
    return;

  mac_xface->get_ue_active_harq_pid(module_idP,CC_id,rnti,
				    frameP,subframeP,
				    &ue_sched_ctl->harq_pid[CC_id],
				    &ue_sched_ctl->round[CC_id],
				    openair_harq_DL);
  if (ue_sched_ctl->ta_timer == 0) {

    // WE SHOULD PROTECT the eNB_UE_stats with a mutex here ...

    ue_sched_ctl->ta_timer = 20;  // wait 20 subframes before taking TA measurement from PHY
    switch (PHY_vars_eNB_g[module_idP][CC_id]->frame_parms.N_RB_DL) {
    case 6:
      ue_sched_ctl->ta_update = eNB_UE_stats->timing_advance_update;
      break;

    case 15:
      ue_sched_ctl->ta_update = eNB_UE_stats->timing_advance_update/2;
      break;

    case 25:
      ue_sched_ctl->ta_update = eNB_UE_stats->timing_advance_update/4;
      break;

    case 50:
      ue_sched_ctl->ta_update = eNB_UE_stats->timing_advance_update/8;
      break;

    case 75:
      ue_sched_ctl->ta_update = eNB_UE_stats->timing_advance_update/12;
      break;

    case 100:
      if (PHY_vars_eNB_g[module_idP][CC_id]->frame_parms.threequarter_fs == 0)
	ue_sched_ctl->ta_update = eNB_UE_stats->timing_advance_update/16;
      else
	ue_sched_ctl->ta_update = eNB_UE_stats->timing_advance_update/12;
      break;
    }
    // clear the update in case PHY does not have a new measurement after timer expiry
    eNB_UE_stats->timing_advance_update =  0;
  }
  else {
    ue_sched_ctl->ta_timer--;
    ue_sched_ctl->ta_update =0; // don't trigger a timing advance command
  }
  if (UE_id==0) {
    VCD_SIGNAL_DUMPER_DUMP_VARIABLE_BY_NAME(VCD_SIGNAL_DUMPER_VARIABLES_UE0_TIMING_ADVANCE,ue_sched_ctl->ta_update);
  }
  nb_rbs_required[CC_id][UE_id]=0;
  ue_sched_ctl->pre_nb_available_rbs[CC_id] = 0;
  ue_sched_ctl->dl_pow_off[CC_id] = 2;
  nb_rbs_required_remaining[CC_id][UE_id] = 0;

  switch (PHY_vars_eNB_g[module_idP][CC_id]->frame_parms.N_RB_DL) {
  case 6:   RBGsize = 1; RBGsize_last = 1; break;
  case 15:  RBGsize = 2; RBGsize_last = 1; break;
  case 25:  RBGsize = 2; RBGsize_last = 1; break;
  case 50:  RBGsize = 3; RBGsize_last = 2; break;
  case 75:  RBGsize = 4; RBGsize_last = 3; break;
  case 100: RBGsize = 4; RBGsize_last = 4; break;
  default: printf("unsupported RBs (%d)\n", PHY_vars_eNB_g[module_idP][CC_id]->frame_parms.N_RB_DL); fflush(stdout); abort();
  }

#ifdef SF0_LIMIT
  switch (N_RBG) {
  case 6:
    sf0_lower=0;
    sf0_upper=5;
    break;
  case 8:
    sf0_lower=2;
    sf0_upper=5;
    break;
  case 13:
    sf0_lower=4;
    sf0_upper=7;
    break;
  case 17:
    sf0_lower=7;
    sf0_upper=9;
    break;
  case 25:
    sf0_lower=11;
    sf0_upper=13;
    break;
  default: printf("unsupported RBs (%d)\n", PHY_vars_eNB_g[module_idP][CC_id]->frame_parms.N_RB_DL); fflush(stdout); abort();
  }
#endif
  // Initialize Subbands according to VRB map
  for (i=0; i<N_RBG; i++) {
    int rb_size = i==N_RBG-1 ? RBGsize_last : RBGsize;

    ue_sched_ctl->rballoc_sub_UE[CC_id][i] = 0;
    rballoc_sub[CC_id][i] = 0;
#ifdef SF0_LIMIT
    // for avoiding 6+ PRBs around DC in subframe 0 (avoid excessive errors)
    /* TODO: make it proper - allocate those RBs, do not "protect" them, but
     * compute number of available REs and limit MCS according to the
     * TBS table 36.213 7.1.7.2.1-1 (can be done after pre-processor)
     */
    if (subframeP==0 &&
	i >= sf0_lower && i <= sf0_upper)
      rballoc_sub[CC_id][i]=1;
#endif
    // for SI-RNTI,RA-RNTI and P-RNTI allocations
    for (j = 0; j < rb_size; j++) {
      if (vrb_map[j+(i*RBGsize)] != 0)  {
	rballoc_sub[CC_id][i] = 1;
	LOG_D(MAC,"Frame %d, subframe %d : vrb %d allocated\n",frameP,subframeP,j+(i*RBGsize));
	break;
      }
    }
    LOG_D(MAC,"Frame %d Subframe %d CC_id %d RBG %i : rb_alloc %d\n",frameP,subframeP,CC_id,i,rballoc_sub[CC_id][i]);
    MIMO_mode_indicator[CC_id][i] = 2;
  }
}

//shibin modify this function to allocate rgb based on priority index
//shibin all the parameters are per cc
void dlsch_scheduler_pre_processor_allocate (module_id_t   Mod_id,
    uint8_t       CC_id,
    int           N_RBG,
    int           transmission_mode,
    int           min_rb_unit,
    uint8_t       N_RB_DL,
    uint16_t      nb_rbs_required[MAX_NUM_CCs][NUMBER_OF_UE_MAX],
    //uint16_t      nb_rbs_required_remaining[MAX_NUM_CCs][NUMBER_OF_UE_MAX],
    unsigned char rballoc_sub[MAX_NUM_CCs][N_RBG_MAX],
    unsigned char MIMO_mode_indicator[MAX_NUM_CCs][N_RBG_MAX],
                                             int UE_id, UE_TEMP_INFO *UE_to_edit)
{
  LOG_D(MAC,"Shibin inside dlsch_scheduler_pre_processor_allocate \n");
  int i, temp_rb = 0;
  rnti_t rnti = UE_RNTI(Mod_id,UE_id);
  LTE_eNB_UE_stats *eNB_UE_stats = mac_xface->get_eNB_UE_stats(Mod_id,CC_id,rnti);

  UE_list_t *UE_list=&eNB_mac_inst[Mod_id].UE_list;
  UE_sched_ctrl *ue_sched_ctl = &UE_list->UE_sched_ctrl[UE_id];

  for(i=0; i<N_RBG; i++) {
    if((rballoc_sub[CC_id][i] == 0)           &&
        (ue_sched_ctl->rballoc_sub_UE[CC_id][i] == 0) &&
        //(nb_rbs_required[CC_id][UE_id]>0)   &&
        (nb_rbs_required[CC_id][UE_id] >=  min_rb_unit-1) &&
        (ue_sched_ctl->pre_nb_available_rbs[CC_id] < nb_rbs_required[CC_id][UE_id])) { // shibin check if this is a problem as now this is updated with every allocation

      // if this UE is not scheduled for TM5
      if (ue_sched_ctl->dl_pow_off[CC_id] != 0 )  {

	if ((i == N_RBG-1) && ((N_RB_DL == 25) || (N_RB_DL == 50))) {

            rballoc_sub[CC_id][i] = 1;
            ue_sched_ctl->rballoc_sub_UE[CC_id][i] = 1;
            MIMO_mode_indicator[CC_id][i] = 1;
            if (transmission_mode == 5 ) {
              ue_sched_ctl->dl_pow_off[CC_id] = 1;
            }
            nb_rbs_required[CC_id][UE_id] = nb_rbs_required[CC_id][UE_id] - min_rb_unit + 1;
            ue_sched_ctl->pre_nb_available_rbs[CC_id] = ue_sched_ctl->pre_nb_available_rbs[CC_id] + min_rb_unit - 1;
            temp_rb += min_rb_unit - 1; // shibin updating local record of rb allocation per ue

        } else {
	  if (nb_rbs_required[CC_id][UE_id] >=  min_rb_unit){
        rballoc_sub[CC_id][i] = 1;
        ue_sched_ctl->rballoc_sub_UE[CC_id][i] = 1;
        MIMO_mode_indicator[CC_id][i] = 1;
	    if (transmission_mode == 5 ) {
	      ue_sched_ctl->dl_pow_off[CC_id] = 1;
	    }
        nb_rbs_required[CC_id][UE_id] = nb_rbs_required[CC_id][UE_id] - min_rb_unit;
	    ue_sched_ctl->pre_nb_available_rbs[CC_id] = ue_sched_ctl->pre_nb_available_rbs[CC_id] + min_rb_unit;
	    temp_rb += min_rb_unit; // shibin updating local record of rb allocation per ue
	  }
	}
      }// dl_pow_off[CC_id][UE_id] ! = 0
    }
  }
  float dummy = UE_to_edit->total_tbs_rate + (mac_xface->get_TBS_DL(eNB_UE_stats->dlsch_mcs1, temp_rb)) / .001;
  UE_to_edit->total_tbs_rate = dummy;
  LOG_I(MAC,"Shibin  calculated value to store for UE %d = %f \n", rnti, dummy);
}


/// ULSCH PRE_PROCESSOR


void ulsch_scheduler_pre_processor(module_id_t module_idP,
                                   int frameP,
                                   sub_frame_t subframeP,
                                   uint16_t *first_rb)
{

  int16_t            i;
  uint16_t           UE_id,n,r;
  uint8_t            CC_id, round, harq_pid;
  uint16_t           nb_allocated_rbs[MAX_NUM_CCs][NUMBER_OF_UE_MAX],total_allocated_rbs[MAX_NUM_CCs],average_rbs_per_user[MAX_NUM_CCs];
  int16_t            total_remaining_rbs[MAX_NUM_CCs];
  uint16_t           max_num_ue_to_be_scheduled=0,total_ue_count=0;
  rnti_t             rnti= -1;
  UE_list_t          *UE_list = &eNB_mac_inst[module_idP].UE_list;
  UE_TEMPLATE        *UE_template = 0;
  LTE_DL_FRAME_PARMS   *frame_parms = 0;


  //LOG_I(MAC,"assign max mcs min rb\n");
  // maximize MCS and then allocate required RB according to the buffer occupancy with the limit of max available UL RB
  assign_max_mcs_min_rb(module_idP,frameP, subframeP, first_rb);

  //LOG_I(MAC,"sort ue \n");
  // sort ues
  sort_ue_ul (module_idP,frameP, subframeP);


  // we need to distribute RBs among UEs
  // step1:  reset the vars
  for (CC_id=0; CC_id<MAX_NUM_CCs; CC_id++) {
    total_allocated_rbs[CC_id]=0;
    total_remaining_rbs[CC_id]=0;
    average_rbs_per_user[CC_id]=0;

    for (i=UE_list->head_ul; i>=0; i=UE_list->next_ul[i]) {
      nb_allocated_rbs[CC_id][i]=0;
    }
  }

  //LOG_I(MAC,"step2 \n");
  // step 2: calculate the average rb per UE
  total_ue_count =0;
  max_num_ue_to_be_scheduled=0;

  for (i=UE_list->head_ul; i>=0; i=UE_list->next_ul[i]) {

    rnti = UE_RNTI(module_idP,i);

    if (rnti==NOT_A_RNTI)
      continue;

    if (UE_list->UE_sched_ctrl[i].ul_out_of_sync == 1)
      continue;

    if (!phy_stats_exist(module_idP, rnti))
      continue;

    UE_id = i;

    for (n=0; n<UE_list->numactiveULCCs[UE_id]; n++) {
      // This is the actual CC_id in the list
      CC_id = UE_list->ordered_ULCCids[n][UE_id];
      UE_template = &UE_list->UE_template[CC_id][UE_id];
      average_rbs_per_user[CC_id]=0;
      frame_parms = mac_xface->get_lte_frame_parms(module_idP,CC_id);

      if (UE_template->pre_allocated_nb_rb_ul > 0) {
        total_ue_count+=1;
      }
      /*
      if((mac_xface->get_nCCE_max(module_idP,CC_id,3,subframeP) - nCCE_to_be_used[CC_id])  > (1<<aggregation)) {
        nCCE_to_be_used[CC_id] = nCCE_to_be_used[CC_id] + (1<<aggregation);
        max_num_ue_to_be_scheduled+=1;
	}*/

      max_num_ue_to_be_scheduled+=1;

      if (total_ue_count == 0) {
        average_rbs_per_user[CC_id] = 0;
      } else if (total_ue_count == 1 ) { // increase the available RBs, special case,
        average_rbs_per_user[CC_id] = frame_parms->N_RB_UL-first_rb[CC_id]+1;
      } else if( (total_ue_count <= (frame_parms->N_RB_DL-first_rb[CC_id])) &&
                 (total_ue_count <= max_num_ue_to_be_scheduled)) {
        average_rbs_per_user[CC_id] = (uint16_t) floor((frame_parms->N_RB_UL-first_rb[CC_id])/total_ue_count);
      } else if (max_num_ue_to_be_scheduled > 0 ) {
        average_rbs_per_user[CC_id] = (uint16_t) floor((frame_parms->N_RB_UL-first_rb[CC_id])/max_num_ue_to_be_scheduled);
      } else {
        average_rbs_per_user[CC_id]=1;
        LOG_W(MAC,"[eNB %d] frame %d subframe %d: UE %d CC %d: can't get average rb per user (should not be here)\n",
              module_idP,frameP,subframeP,UE_id,CC_id);
      }
    }
  }
  if (total_ue_count > 0)
    LOG_D(MAC,"[eNB %d] Frame %d subframe %d: total ue to be scheduled %d/%d\n",
	  module_idP, frameP, subframeP,total_ue_count, max_num_ue_to_be_scheduled);

  //LOG_D(MAC,"step3\n");

  // step 3: assigne RBS
  for (i=UE_list->head_ul; i>=0; i=UE_list->next_ul[i]) {
    rnti = UE_RNTI(module_idP,i);

    if (rnti==NOT_A_RNTI)
      continue;
    if (UE_list->UE_sched_ctrl[i].ul_out_of_sync == 1)
      continue;
    if (!phy_stats_exist(module_idP, rnti))
      continue;

    UE_id = i;
    // shibin this is where the actual allocation of rbs take place based on the min of average rb per user and requested prb
    for (n=0; n<UE_list->numactiveULCCs[UE_id]; n++) {
      // This is the actual CC_id in the list
      CC_id = UE_list->ordered_ULCCids[n][UE_id];

      mac_xface->get_ue_active_harq_pid(module_idP,CC_id,rnti,frameP,subframeP,&harq_pid,&round,openair_harq_UL);

      if(round>0) {
        nb_allocated_rbs[CC_id][UE_id] = UE_list->UE_template[CC_id][UE_id].nb_rb_ul[harq_pid]; // shibin this is for retransmissions
      } else {
        nb_allocated_rbs[CC_id][UE_id] = cmin(UE_list->UE_template[CC_id][UE_id].pre_allocated_nb_rb_ul, average_rbs_per_user[CC_id]); // shibin replace this with new logic
      }

      total_allocated_rbs[CC_id]+= nb_allocated_rbs[CC_id][UE_id];

    }
  }

  // step 4: assign the remaining RBs and set the pre_allocated rbs accordingly
  for(r=0; r<2; r++) {

    for (i=UE_list->head_ul; i>=0; i=UE_list->next_ul[i]) {
      rnti = UE_RNTI(module_idP,i);

      if (rnti==NOT_A_RNTI)
        continue;
      if (UE_list->UE_sched_ctrl[i].ul_out_of_sync == 1)
	continue;
      if (!phy_stats_exist(module_idP, rnti))
        continue;

      UE_id = i;

      for (n=0; n<UE_list->numactiveULCCs[UE_id]; n++) {
        // This is the actual CC_id in the list
        CC_id = UE_list->ordered_ULCCids[n][UE_id];
        UE_template = &UE_list->UE_template[CC_id][UE_id];
        frame_parms = mac_xface->get_lte_frame_parms(module_idP,CC_id);
        total_remaining_rbs[CC_id]=frame_parms->N_RB_UL - first_rb[CC_id] - total_allocated_rbs[CC_id];

        if (total_ue_count == 1 ) {
          total_remaining_rbs[CC_id]+=1;
        }

        if ( r == 0 ) {
          while ( (UE_template->pre_allocated_nb_rb_ul > 0 ) &&
                  (nb_allocated_rbs[CC_id][UE_id] < UE_template->pre_allocated_nb_rb_ul) &&
                  (total_remaining_rbs[CC_id] > 0)) {
            nb_allocated_rbs[CC_id][UE_id] = cmin(nb_allocated_rbs[CC_id][UE_id]+1,UE_template->pre_allocated_nb_rb_ul);
            total_remaining_rbs[CC_id]--;
            total_allocated_rbs[CC_id]++;
          }
        } else {
          UE_template->pre_allocated_nb_rb_ul= nb_allocated_rbs[CC_id][UE_id];
          LOG_D(MAC,"******************UL Scheduling Information for UE%d CC_id %d ************************\n",UE_id, CC_id);
          LOG_D(MAC,"[eNB %d] total RB allocated for UE%d CC_id %d  = %d\n", module_idP, UE_id, CC_id, UE_template->pre_allocated_nb_rb_ul);
        }
      }
    }
  }

  for (CC_id=0; CC_id<MAX_NUM_CCs; CC_id++) {
    frame_parms= mac_xface->get_lte_frame_parms(module_idP,CC_id);

    if (total_allocated_rbs[CC_id]>0) {
      LOG_D(MAC,"[eNB %d] total RB allocated for all UEs = %d/%d\n", module_idP, total_allocated_rbs[CC_id], frame_parms->N_RB_UL - first_rb[CC_id]);
    }
  }
}


void assign_max_mcs_min_rb(module_id_t module_idP,int frameP, sub_frame_t subframeP, uint16_t *first_rb)
{

  int                i;
  uint16_t           n,UE_id;
  uint8_t            CC_id;
  rnti_t             rnti           = -1;
  int                mcs;
  int                rb_table_index=0,tbs,tx_power;
  eNB_MAC_INST       *eNB = &eNB_mac_inst[module_idP];
  UE_list_t          *UE_list = &eNB->UE_list;

  UE_TEMPLATE       *UE_template;
  LTE_DL_FRAME_PARMS   *frame_parms;


  for (i = 0; i < NUMBER_OF_UE_MAX; i++) {
    if (UE_list->active[i] != TRUE) continue;

    rnti = UE_RNTI(module_idP,i);

    if (rnti==NOT_A_RNTI)
      continue;
    if (UE_list->UE_sched_ctrl[i].ul_out_of_sync == 1)
      continue;
    if (!phy_stats_exist(module_idP, rnti))
      continue;

    if (UE_list->UE_sched_ctrl[i].phr_received == 1)
      mcs = 20; // if we've received the power headroom information the UE, we can go to maximum mcs
              //Shibin- (Power headroom indicates how much transmission power left for a UE to use in addition to the power being used by current transmission)
    else
      mcs = 10; // otherwise, limit to QPSK PUSCH

    UE_id = i;

    for (n=0; n<UE_list->numactiveULCCs[UE_id]; n++) {
      // This is the actual CC_id in the list
      CC_id = UE_list->ordered_ULCCids[n][UE_id];

      if (CC_id >= MAX_NUM_CCs) {
        LOG_E( MAC, "CC_id %u should be < %u, loop n=%u < numactiveULCCs[%u]=%u",
               CC_id,
               MAX_NUM_CCs,
               n,
               UE_id,
               UE_list->numactiveULCCs[UE_id]);
      }

      AssertFatal( CC_id < MAX_NUM_CCs, "CC_id %u should be < %u, loop n=%u < numactiveULCCs[%u]=%u",
                   CC_id,
                   MAX_NUM_CCs,
                   n,
                   UE_id,
                   UE_list->numactiveULCCs[UE_id]);
      frame_parms=mac_xface->get_lte_frame_parms(module_idP,CC_id);
      UE_template = &UE_list->UE_template[CC_id][UE_id];

      // if this UE has UL traffic
      if (UE_template->ul_total_buffer > 0 ) {

        /* start with 3 RB => rb_table_index = 2 */
        rb_table_index = 2;
        tbs = mac_xface->get_TBS_UL(mcs,3);  // 1 or 2 PRB with cqi enabled does not work well!
        // fixme: set use_srs flag
        tx_power= mac_xface->estimate_ue_tx_power(tbs,rb_table[rb_table_index],0,frame_parms->Ncp,0);
        // shibin -  adjust the mcs value untill tx-power is below the phr_info and buffer is less than total available buffer, this logic maximizes the possible mcs value for that ue
        while ((((UE_template->phr_info - tx_power) < 0 ) || (tbs > UE_template->ul_total_buffer))&&
               (mcs > 3)) {
          // LOG_I(MAC,"UE_template->phr_info %d tx_power %d mcs %d\n", UE_template->phr_info,tx_power, mcs);
          mcs--;
          tbs = mac_xface->get_TBS_UL(mcs,rb_table[rb_table_index]);
          tx_power = mac_xface->estimate_ue_tx_power(tbs,rb_table[rb_table_index],0,frame_parms->Ncp,0); // fixme: set use_srs
        }
        // shibin- this module minimizes the possible prb allocation
        while ((tbs < UE_template->ul_total_buffer) &&
               (rb_table[rb_table_index]<(frame_parms->N_RB_UL-first_rb[CC_id])) &&
               ((UE_template->phr_info - tx_power) > 0) &&
               (rb_table_index < 32 )) {
          //  LOG_I(MAC,"tbs %d ul buffer %d rb table %d max ul rb %d\n", tbs, UE_template->ul_total_buffer, rb_table[rb_table_index], frame_parms->N_RB_UL-first_rb[CC_id]);
          rb_table_index++;
          tbs = mac_xface->get_TBS_UL(mcs,rb_table[rb_table_index]);
          tx_power = mac_xface->estimate_ue_tx_power(tbs,rb_table[rb_table_index],0,frame_parms->Ncp,0);
        }

        UE_template->ue_tx_power = tx_power;
        // shibin- reduce prb allocation if its greater than the available prb for that particular common channel for the ue
        if (rb_table[rb_table_index]>(frame_parms->N_RB_UL-first_rb[CC_id]-1)) {
          rb_table_index--;
        }

        // 1 or 2 PRB with cqi enabled does not work well!
	if (rb_table[rb_table_index]<3) {
          rb_table_index=2; //3PRB
        }

        UE_template->pre_assigned_mcs_ul=mcs;
        UE_template->pre_allocated_rb_table_index_ul=rb_table_index;
        UE_template->pre_allocated_nb_rb_ul= rb_table[rb_table_index];
        LOG_D(MAC,"[eNB %d] frame %d subframe %d: for UE %d CC %d: pre-assigned mcs %d, pre-allocated rb_table[%d]=%d RBs (phr %d, tx power %d)\n",
              module_idP, frameP, subframeP, UE_id, CC_id,
              UE_template->pre_assigned_mcs_ul,
              UE_template->pre_allocated_rb_table_index_ul,
              UE_template->pre_allocated_nb_rb_ul,
              UE_template->phr_info,tx_power);
      } else {
        /* if UE has pending scheduling request then pre-allocate 3 RBs */
        //if (UE_template->ul_active == 1 && UE_template->ul_SR == 1) {
        if (UE_is_to_be_scheduled(module_idP, CC_id, i)) {
          UE_template->pre_allocated_rb_table_index_ul = 2;
          UE_template->pre_allocated_nb_rb_ul          = 3;
        } else {
          UE_template->pre_allocated_rb_table_index_ul=-1;
          UE_template->pre_allocated_nb_rb_ul=0;
        }
      }
    }
  }
}

struct sort_ue_ul_params {
  int module_idP;
  int frameP;
  int subframeP;
};

static int ue_ul_compare(const void *_a, const void *_b, void *_params)
{
  struct sort_ue_ul_params *params = _params;
  UE_list_t *UE_list = &eNB_mac_inst[params->module_idP].UE_list;

  int UE_id1 = *(const int *)_a;
  int UE_id2 = *(const int *)_b;

  int rnti1  = UE_RNTI(params->module_idP, UE_id1);
  int pCCid1 = UE_PCCID(params->module_idP, UE_id1);
  int round1 = maxround(params->module_idP, rnti1, params->frameP, params->subframeP, 1);

  int rnti2  = UE_RNTI(params->module_idP, UE_id2);
  int pCCid2 = UE_PCCID(params->module_idP, UE_id2);
  int round2 = maxround(params->module_idP, rnti2, params->frameP, params->subframeP, 1);

  if (round1 > round2) return -1;
  if (round1 < round2) return 1;

  if (UE_list->UE_template[pCCid1][UE_id1].ul_buffer_info[LCGID0] > UE_list->UE_template[pCCid2][UE_id2].ul_buffer_info[LCGID0])
    return -1;
  if (UE_list->UE_template[pCCid1][UE_id1].ul_buffer_info[LCGID0] < UE_list->UE_template[pCCid2][UE_id2].ul_buffer_info[LCGID0])
    return 1;

  if (UE_list->UE_template[pCCid1][UE_id1].ul_total_buffer > UE_list->UE_template[pCCid2][UE_id2].ul_total_buffer)
    return -1;
  if (UE_list->UE_template[pCCid1][UE_id1].ul_total_buffer < UE_list->UE_template[pCCid2][UE_id2].ul_total_buffer)
    return 1;

  if (UE_list->UE_template[pCCid1][UE_id1].pre_assigned_mcs_ul > UE_list->UE_template[pCCid2][UE_id2].pre_assigned_mcs_ul)
    return -1;
  if (UE_list->UE_template[pCCid1][UE_id1].pre_assigned_mcs_ul < UE_list->UE_template[pCCid2][UE_id2].pre_assigned_mcs_ul)
    return 1;

  return 0;

}

void sort_ue_ul (module_id_t module_idP,int frameP, sub_frame_t subframeP)
{
  int               i;
  int               list[NUMBER_OF_UE_MAX];
  int               list_size = 0;
  int               rnti;
  struct sort_ue_ul_params params = { module_idP, frameP, subframeP };

  UE_list_t *UE_list = &eNB_mac_inst[module_idP].UE_list;

  for (i = 0; i < NUMBER_OF_UE_MAX; i++) {
    rnti = UE_RNTI(module_idP, i);
    if (rnti == NOT_A_RNTI)
      continue;
    if (UE_list->UE_sched_ctrl[i].ul_out_of_sync == 1)
      continue;
    if (!phy_stats_exist(module_idP, rnti))
      continue;
    list[list_size] = i;
    list_size++;
  }

  qsort_r(list, list_size, sizeof(int), ue_ul_compare, &params);

  if (list_size) {
    for (i = 0; i < list_size-1; i++)
      UE_list->next_ul[list[i]] = list[i+1];
    UE_list->next_ul[list[list_size-1]] = -1;
    UE_list->head_ul = list[0];
  } else {
    UE_list->head_ul = -1;
  }
}
