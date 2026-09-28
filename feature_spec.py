# GENERATED FILE - do not hand-edit.
# Produced by generate_feature_spec.py from feature_spec.json.
# Re-run that script after feature_spec.json changes; `make
# check-feature-spec-fresh` fails the build if this has drifted.

NUM_INPUTS = 74

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
        3
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

SCALER_MEAN = [0.17363333702087402, 0.9257222414016724, 6.105133533477783, 60.4566650390625, 0.09647161513566971, 0.0964268147945404, 4.4710031943395734e-05, 60.25248718261719, 60.4566650390625, -0.2041497826576233, 12.23253345489502, 12.370288848876953, 2.272311210632324, 0.744688868522644, 83.07856750488281, 25.46066665649414, 6.817055702209473, 0.10719554871320724, 0.1072511076927185, -0.0071444446220994, 0.13348889350891113, 0.20000000298023224, 0.20000000298023224, 0.20000000298023224, 0.20000000298023224, 0.20000000298023224, 0.015377777628600597, 0.31194445490837097, 0.06886667013168335, 0.6038110852241516, 0.015377777628600597, 0.30418887734413147, 0.07785555720329285, 0.602577805519104, 0.20000000298023224, 0.4000000059604645, 0.4000000059604645, 0.8375333547592163, 0.00837777741253376, 0.022477777674794197, 0.06857778131961823, 0.0630333349108696, 0.31727778911590576, 0.021166667342185974, 0.0722000002861023, 0.021788889542222023, 0.16583333909511566, 0.05290000140666962, 0.029111111536622047, 0.047155555337667465, 0.03896666690707207, 0.23360000550746918, 0.0, 0.800000011920929, 0.20000000298023224, 0.4000000059604645, 0.0, 0.20000000298023224, 0.20000000298023224, 0.6000000238418579, 0.20000000298023224, 0.0016222222475335002, 0.0, 0.15203332901000977, 0.006200000178068876, 0.01695346087217331, 0.029280563816428185, 0.2568998336791992, 0.019762255251407623, 0.030038736760616302, 0.02076118253171444, 0.0007106509292498231, 0.2806444466114044, 0.0]

SCALER_STD = [0.4128711223602295, 2.4181408882141113, 3.3872690200805664, 17.75758171081543, 0.22694692015647888, 0.22874420881271362, 0.03800566494464874, 17.699071884155273, 17.75758171081543, 0.682780385017395, 27.389759063720703, 32.4179801940918, 0.9444116353988647, 1.2816026210784912, 30.17692756652832, 35.70536804199219, 5.666077136993408, 0.24372297525405884, 0.260174036026001, 0.09406889230012894, 0.5391378402709961, 0.4000000059604645, 0.4000000059604645, 0.4000000059604645, 0.4000000059604645, 0.4000000059604645, 0.12304999679327011, 0.4632872939109802, 0.25322726368904114, 0.4891045391559601, 0.12304999679327011, 0.46006307005882263, 0.2679441273212433, 0.48936471343040466, 0.4000000059604645, 0.4898979663848877, 0.4898979663848877, 0.3688783645629883, 0.09114598482847214, 0.14823132753372192, 0.2527347803115845, 0.24302290380001068, 0.4654165804386139, 0.14393970370292664, 0.25881877541542053, 0.14599360525608063, 0.37193095684051514, 0.2238338440656662, 0.1681179702281952, 0.2119714915752411, 0.19351555407047272, 0.4231205880641937, 1.0, 0.4000000059604645, 0.4000000059604645, 0.4898979663848877, 1.0, 0.4000000059604645, 0.4000000059604645, 0.4898979365825653, 0.4000000059604645, 0.04024413973093033, 1.0, 0.35905319452285767, 0.07849560678005219, 0.05804026871919632, 0.05922115594148636, 0.4137754738330841, 0.07743912935256958, 0.09469811618328094, 0.06286747753620148, 0.009165197610855103, 0.44931408762931824, 1.0]

PATH_COST_SENTINEL_REPLACEMENT = 40.40251018385996
