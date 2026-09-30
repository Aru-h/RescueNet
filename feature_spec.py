# GENERATED FILE - do not hand-edit.
# Produced by generate_feature_spec.py from feature_spec.json.
# Re-run that script after feature_spec.json changes; `make
# check-feature-spec-fresh` fails the build if this has drifted.

NUM_INPUTS = 75

INPUTS = [
    "incident_priority",
    "victim_count",
    "hours_worked",
    "stamina",
    "twin_expected_progress",
    "twin_actual_progress",
    "twin_progress_error",
    "twin_expected_stamina",
    "twin_actual_stamina",
    "twin_stamina_error",
    "twin_remaining_path_cost",
    "twin_expected_work_remaining",
    "twin_mission_stage",
    "twin_communication_health",
    "equipment_level",
    "team_zone_trust",
    "env_raw_travel_cost",
    "stall_frac_last_10",
    "stall_frac_last_5",
    "delta_priority",
    "delta_victim_count",
    "specialization_0",
    "specialization_1",
    "specialization_2",
    "specialization_3",
    "specialization_4",
    "twin_expected_operating_state_0",
    "twin_expected_operating_state_1",
    "twin_expected_operating_state_2",
    "twin_expected_operating_state_3",
    "status_0",
    "status_1",
    "status_2",
    "status_3",
    "fitness_tier_0",
    "fitness_tier_1",
    "fitness_tier_2",
    "incident_req_capability_-1",
    "incident_req_capability_0",
    "incident_req_capability_1",
    "incident_req_capability_2",
    "incident_req_capability_3",
    "incident_req_capability_4",
    "team_zone_id_0",
    "team_zone_id_1",
    "team_zone_id_2",
    "team_zone_id_3",
    "team_zone_id_4",
    "team_zone_id_5",
    "team_zone_id_6",
    "team_zone_id_7",
    "team_zone_id_8",
    "team_zone_id_9",
    "has_support",
    "equipment_rope",
    "equipment_spreader",
    "equipment_medical_kit",
    "equipment_boat",
    "equipment_k9",
    "equipment_hazmat_suit",
    "equipment_structural",
    "equipment_generator",
    "hazard_gas",
    "hazard_flood",
    "hazard_structural",
    "hazard_cbrn",
    "evidence_progress",
    "evidence_stamina",
    "evidence_communication",
    "evidence_operating_state",
    "evidence_path",
    "evidence_assignment",
    "evidence_timeout",
    "confirmed_mobility_impairment",
    "path_is_unreachable"
]

VOCABULARIES = {
    "specialization": [
        0,
        1,
        2,
        3,
        4
    ],
    "twin_expected_operating_state": [
        0,
        1,
        2,
        3
    ],
    "status": [
        0,
        1,
        2,
        3
    ],
    "fitness_tier": [
        0,
        1,
        2
    ],
    "incident_req_capability": [
        -1,
        0,
        1,
        2,
        3,
        4
    ],
    "team_zone_id": [
        0,
        1,
        2,
        3,
        4,
        5,
        6,
        7,
        8,
        9
    ]
}

SCALER_MEAN = [0.1123381182551384, 0.5335856676101685, 19.758947372436523, 54.256771087646484, 0.08796700835227966, 0.08616849780082703, 0.0017985198646783829, 54.02098846435547, 54.256771087646484, -0.23577742278575897, 15.01734447479248, 7.229304313659668, 2.256639242172241, 0.7307881116867065, 85.70367431640625, 19.01329231262207, 6.4826154708862305, 0.07304524630308151, 0.07306040823459625, -0.013158840127289295, 0.10378710180521011, 0.20000000298023224, 0.20000000298023224, 0.20000000298023224, 0.20000000298023224, 0.20000000298023224, 0.02493739128112793, 0.319026380777359, 0.03805217519402504, 0.6179840564727783, 0.02493739128112793, 0.31486189365386963, 0.04292057827115059, 0.617280125617981, 0.20000000298023224, 0.4000000059604645, 0.4000000059604645, 0.905868411064148, 0.01078579667955637, 0.019742753356695175, 0.0345427542924881, 0.02906028926372528, 0.0, 0.28002581000328064, 0.025830144062638283, 0.04647260904312134, 0.038884636014699936, 0.24717362225055695, 0.03955753520131111, 0.09631391614675522, 0.044822897762060165, 0.0858769565820694, 0.09504188597202301, 1.8115941202268004e-05, 0.800000011920929, 0.20000000298023224, 0.4000000059604645, 0.0, 0.20000000298023224, 0.20000000298023224, 0.6000000238418579, 0.20000000298023224, 0.0027836232911795378, 0.0, 0.07852912694215775, 0.006775796879082918, 0.01356285810470581, 0.038096215575933456, 0.25249528884887695, 0.012462354265153408, 0.03048355132341385, 0.018437838181853294, 0.0009315332281403244, 0.30576059222221375, 0.0]

SCALER_STD = [0.3807467818260193, 1.9981465339660645, 11.634356498718262, 14.327730178833008, 0.21672365069389343, 0.2144402414560318, 0.04036656767129898, 14.269598007202148, 14.327730178833008, 0.7788868546485901, 35.456722259521484, 24.954805374145508, 0.9814988374710083, 1.2738710641860962, 27.973957061767578, 33.78456497192383, 5.029601573944092, 0.1949123740196228, 0.20867183804512024, 0.12787632644176483, 0.5273911356925964, 0.4000000059604645, 0.4000000059604645, 0.4000000059604645, 0.4000000059604645, 0.4000000059604645, 0.15593433380126953, 0.4660992920398712, 0.19132225215435028, 0.48588043451309204, 0.15593433380126953, 0.4644608497619629, 0.20267808437347412, 0.4860508143901825, 0.4000000059604645, 0.4898979365825653, 0.4898979365825653, 0.29201170802116394, 0.10329309105873108, 0.13911497592926025, 0.18261858820915222, 0.1679755598306656, 1.0, 0.44901150465011597, 0.15862832963466644, 0.21050631999969482, 0.19331999123096466, 0.43136855959892273, 0.1949172466993332, 0.29502126574516296, 0.2069149613380432, 0.28018227219581604, 0.2932727634906769, 0.004256243817508221, 0.4000000059604645, 0.4000000059604645, 0.4898979365825653, 1.0, 0.4000000059604645, 0.4000000059604645, 0.4898979663848877, 0.4000000059604645, 0.05268656462430954, 1.0, 0.26900243759155273, 0.08203587681055069, 0.05727769806981087, 0.0693708211183548, 0.41167983412742615, 0.05934221297502518, 0.09563624858856201, 0.05914323031902313, 0.01130124181509018, 0.46072882413864136, 1.0]

PATH_COST_SENTINEL_REPLACEMENT = 47.81393622205077
