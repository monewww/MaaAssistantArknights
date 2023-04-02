#include "CombatRecordRecognitionTask.h"

#include "Config/Miscellaneous/BattleDataConfig.h"
#include "Config/Miscellaneous/TilePack.h"
#include "Config/TaskData.h"
#include "Utils/ImageIo.hpp"
#include "Utils/Logger.hpp"
#include "Utils/NoWarningCV.h"
#include "Utils/Ranges.hpp"
#include "Vision/Battle/BattleDeployDirectionImageAnalyzer.h"
#include "Vision/Battle/BattleFormationImageAnalyzer.h"
#include "Vision/Battle/BattleImageAnalyzer.h"
#include "Vision/Battle/BattleOperatorsImageAnalyzer.h"
#include "Vision/Battle/BattleSkillReadyImageAnalyzer.h"
#include "Vision/BestMatchImageAnalyzer.h"
#include "Vision/OcrWithPreprocessImageAnalyzer.h"

#include <unordered_map>
#include <unordered_set>

bool asst::CombatRecordRecognitionTask::set_video_path(const std::filesystem::path& path)
{
    if (!std::filesystem::exists(path)) {
        Log.error(__FUNCTION__, "filename not exists", path);
        return false;
    }
    m_video_path = path;
    return true;
}

bool asst::CombatRecordRecognitionTask::set_stage_name(const std::string& stage_name)
{
    m_stage_name = stage_name;
    return true;
}

bool asst::CombatRecordRecognitionTask::_run()
{
    LogTraceFunction;

    auto release_video = [](cv::VideoCapture* video) {
        if (video && video->isOpened()) {
            video->release();
        }
    };
    auto u8_path = utils::path_to_utf8_string(m_video_path);
    m_video_ptr = std::shared_ptr<cv::VideoCapture>(new cv::VideoCapture(u8_path), release_video);

    if (!m_video_ptr->isOpened()) {
        Log.error(__FUNCTION__, "video_io open failed", m_video_path);
        return false;
    }
    m_video_fps = m_video_ptr->get(cv::CAP_PROP_FPS);
    m_video_frame_count = static_cast<size_t>(m_video_ptr->get(cv::CAP_PROP_FRAME_COUNT));
    m_battle_start_frame = 0;
    m_scale = WindowHeightDefault / m_video_ptr->get(cv::CAP_PROP_FRAME_HEIGHT);

#ifdef ASST_DEBUG
    cv::namedWindow(DrawWindow, cv::WINDOW_AUTOSIZE);
#endif // ASST_DEBUG

    if (!analyze_formation()) {
        Log.error(__FUNCTION__, "failed to analyze formation");
        return false;
    }

    if (!analyze_stage()) {
        Log.error(__FUNCTION__, "unknown stage");
        return false;
    }

    if (!analyze_deployment()) {
        Log.error(__FUNCTION__, "failed to match deployment");
        return false;
    }

    if (!slice_video()) {
        Log.error(__FUNCTION__, "failed to slice");
        return false;
    }

    if (!analyze_all_clips()) {
        Log.error(__FUNCTION__, "failed to analyze clips");
        return false;
    }

    Log.info("full copilot json", m_copilot_json.to_string());

    std::string filename = "MaaAI_" + m_stage_name + "_" + utils::path_to_utf8_string(m_video_path.stem()) + "_" +
                           utils::get_time_filestem() + ".json";
    auto filepath = UserDir.get() / "cache" / "CombatRecord" / utils::path(filename);
    std::filesystem::create_directories(filepath.parent_path());
    std::ofstream osf(filepath);
    osf << m_copilot_json.format();
    osf.close();

    auto cb_json = basic_info_with_what("Finished");
    cb_json["details"]["filename"] = utils::path_to_utf8_string(filepath);
    callback(AsstMsg::SubTaskExtraInfo, cb_json);

#ifdef ASST_DEBUG
    cv::destroyWindow(DrawWindow);
#endif // ASST_DEBUG

    return true;
}

bool asst::CombatRecordRecognitionTask::analyze_formation()
{
    LogTraceFunction;
    callback(AsstMsg::SubTaskStart, basic_info_with_what("OcrFormation"));

    const int skip_count = m_video_fps > m_formation_fps ? static_cast<int>(m_video_fps / m_formation_fps) : 1;

    BattleFormationImageAnalyzer formation_ananlyzer;
    int no_changes_count = 0;
    for (size_t i = 0; i < m_video_frame_count; i += skip_frames(skip_count)) {
        cv::Mat frame;
        *m_video_ptr >> frame;
        if (frame.empty()) {
            Log.error(i, "frame is empty");
            callback(AsstMsg::SubTaskError, basic_info_with_what("OcrFormation"));
            return false;
        }

        cv::resize(frame, frame, cv::Size(), m_scale, m_scale, cv::INTER_AREA);

        formation_ananlyzer.set_image(frame);
        bool analyzed = formation_ananlyzer.analyze();
        show_img(formation_ananlyzer);
        // 有些视频会有个过渡或者动画啥的，只取一帧识别的可能不全。多识别几帧
        if (analyzed) {
            const auto& cur = formation_ananlyzer.get_result();
            if (cur.size() > m_formation.size()) {
                m_formation = cur;
            }
            else if (++no_changes_count > 5) {
                m_formation_end_frame = i;
                break;
            }
        }
        else if (!m_formation.empty()) {
            m_formation_end_frame = i;
            break;
        }
    }

    Log.info("Formation:", m_formation | views::keys);
    auto cb_info = basic_info_with_what("OcrFormation");
    auto& cb_formation = cb_info["details"]["formation"];
    for (const auto& [name, avatar] : m_formation) {
        std::vector<battle::OperUsage> opers;
        opers.emplace_back(battle::OperUsage { name, 0, battle::SkillUsage::NotUse });
        json::object oper_json { { "name", name }, { "skill", 0 }, { "skill_usage", 0 } };
        m_copilot_json["opers"].array_emplace(std::move(oper_json));

        cb_formation.array_emplace(name);
        asst::imwrite(utils::path("debug/video_export/formation/") / utils::path(name + ".png"), avatar);
    }
    callback(AsstMsg::SubTaskCompleted, cb_info);

    return true;
}

bool asst::CombatRecordRecognitionTask::analyze_stage()
{
    LogTraceFunction;

    if (m_stage_name.empty()) {
        callback(AsstMsg::SubTaskStart, basic_info_with_what("OcrStage"));

        const auto stage_name_task_ptr = Task.get("BattleStageName");
        const int skip_count = m_video_fps > m_stage_ocr_fps ? static_cast<int>(m_video_fps / m_stage_ocr_fps) : 1;

        for (size_t i = m_formation_end_frame; i < m_video_frame_count; i += skip_frames(skip_count)) {
            cv::Mat frame;
            *m_video_ptr >> frame;
            if (frame.empty()) {
                Log.error(i, "frame is empty");
                callback(AsstMsg::SubTaskError, basic_info_with_what("OcrStage"));
                return false;
            }

            cv::resize(frame, frame, cv::Size(), m_scale, m_scale, cv::INTER_AREA);

            OcrWithPreprocessImageAnalyzer stage_analyzer(frame);
            stage_analyzer.set_task_info(stage_name_task_ptr);
            bool analyzed = stage_analyzer.analyze();
            show_img(stage_analyzer);

            if (!analyzed) {
                BattleImageAnalyzer battle_analyzer(frame);
                if (battle_analyzer.analyze()) {
                    Log.error(i, "already start button, but still failed to analyze stage name");
                    m_stage_ocr_end_frame = i;
                    callback(AsstMsg::SubTaskError, basic_info_with_what("OcrStage"));
                    return false;
                }
                continue;
            }
            stage_analyzer.sort_result_by_score();
            const std::string& text = stage_analyzer.get_result().front().text;

            if (text.empty() || !Tile.contains(text)) {
                continue;
            }

            m_stage_name = text;
            m_stage_ocr_end_frame = i;
            break;
        }
    }

    Log.info("Stage", m_stage_name);
    if (m_stage_name.empty() || !Tile.contains(m_stage_name)) {
        callback(AsstMsg::SubTaskError, basic_info_with_what("OcrStage"));
        return false;
    }
    m_normal_tile_info = Tile.calc(m_stage_name, false);

    m_copilot_json["stage_name"] = m_stage_name;
    m_copilot_json["minimum_required"] = "v4.0.0";
    m_copilot_json["doc"]["title"] = "MAA AI - " + m_stage_name;
    m_copilot_json["doc"]["details"] =
        "Built at: " + utils::get_format_time() + "\n" + utils::path_to_utf8_string(m_video_path);

    callback(AsstMsg::SubTaskCompleted, basic_info_with_what("OcrStage"));
    return true;
}

bool asst::CombatRecordRecognitionTask::analyze_deployment()
{
    LogTraceFunction;
    callback(AsstMsg::SubTaskStart, basic_info_with_what("MatchDeployment"));

    const int skip_count = m_video_fps > m_deployment_fps ? static_cast<int>(m_video_fps / m_deployment_fps) : 1;

    BattleImageAnalyzer oper_analyzer;
    oper_analyzer.set_target(BattleImageAnalyzer::Target::Oper | BattleImageAnalyzer::Target::PauseButton);

    for (size_t i = m_stage_ocr_end_frame; i < m_video_frame_count; i += skip_frames(skip_count)) {
        cv::Mat frame;
        *m_video_ptr >> frame;
        if (frame.empty()) {
            Log.error(i, "frame is empty");
            callback(AsstMsg::SubTaskError, basic_info_with_what("MatchDeployment"));
            return false;
        }

        cv::resize(frame, frame, cv::Size(), m_scale, m_scale, cv::INTER_AREA);

        oper_analyzer.set_image(frame);
        bool analyzed = oper_analyzer.analyze();
        show_img(oper_analyzer);
        if (analyzed) {
            m_battle_start_frame = i;
            break;
        }
    }
    const auto& deployment = oper_analyzer.get_opers();

    auto avatar_task_ptr = Task.get("BattleAvatarDataForFormation");
    for (const auto& [name, formation_avatar] : m_formation) {
        BestMatchImageAnalyzer best_match_analyzer(formation_avatar);
        best_match_analyzer.set_task_info(avatar_task_ptr);

        std::unordered_set<battle::Role> roles = { BattleData.get_role(name) };
        if (name == "阿米娅") {
            roles.emplace(battle::Role::Warrior);
        }

        // 编队界面，有些视频会有些花里胡哨的特效遮挡啥的，所以尽量减小点模板尺寸
        auto crop_roi = make_rect<cv::Rect>(avatar_task_ptr->rect_move);
        // 小车的缩放太离谱了
        const size_t scale_ends = BattleData.get_rarity(name) == 1 ? 200 : 125;
        std::unordered_map<std::string, cv::Mat> candidate;
        for (const auto& oper : deployment) {
            if (!roles.contains(oper.role)) {
                continue;
            }
            cv::Mat crop_avatar = oper.avatar(crop_roi);
            // 从编队到待部署区，每个干员的缩放大小都不一样，暴力跑一遍
            // TODO: 不知道gamedata里有没有这个缩放数据，直接去拿
            for (size_t i = 100; i < scale_ends; ++i) {
                double avatar_scale = i / 100.0;
                const auto resize_method = avatar_scale < 1.0 ? cv::INTER_AREA : cv::INTER_LINEAR;
                cv::Mat resized_avatar;
                cv::resize(crop_avatar, resized_avatar, cv::Size(), avatar_scale, avatar_scale, resize_method);
                std::string flag = name + "|" + std::to_string(oper.index) + "|" + std::to_string(i);
                best_match_analyzer.append_templ(flag, resized_avatar);
                candidate.emplace(flag, oper.avatar);
            }
        }
        bool analyzed = best_match_analyzer.analyze();
        // show_img(best_match_analyzer);
        if (!analyzed) {
            Log.warn(m_battle_start_frame, "failed to match", name);
            continue;
        }
        m_all_avatars.emplace(name, candidate.at(best_match_analyzer.get_result().name));
    }
    callback(AsstMsg::SubTaskCompleted, basic_info_with_what("MatchDeployment"));

    return !m_all_avatars.empty();
}

bool asst::CombatRecordRecognitionTask::slice_video()
{
    LogTraceFunction;
    callback(AsstMsg::SubTaskStart, basic_info_with_what("Slice"));

    const int skip_count = m_video_fps > m_deployment_fps ? static_cast<int>(m_video_fps / m_deployment_fps) : 1;

    int not_in_battle_count = 0;
    bool in_segment = false;

#ifdef ASST_DEBUG
    cv::Mat pre_frame;
#endif

    for (size_t i = m_battle_start_frame; i < m_video_frame_count; i += skip_frames(skip_count)) {
        cv::Mat frame;
        *m_video_ptr >> frame;
        if (frame.empty()) {
            Log.error(i, "frame is empty");
            callback(AsstMsg::SubTaskError, basic_info_with_what("Slice"));
            return false;
        }

        cv::resize(frame, frame, cv::Size(), m_scale, m_scale, cv::INTER_AREA);

        BattleImageAnalyzer oper_analyzer(frame);
        oper_analyzer.set_target(BattleImageAnalyzer::Target::Oper | BattleImageAnalyzer::Target::DetailPage);
        bool analyzed = oper_analyzer.analyze();
        show_img(oper_analyzer);

        if (!analyzed) {
            if (in_segment && !m_clips.empty()) {
                m_clips.back().end_frame = i;
            }
            in_segment = false;

            if (m_battle_end_frame == 0) {
                m_battle_end_frame = i;
            }
            if (++not_in_battle_count > 10) {
                break;
            }
            continue;
        }
        m_battle_end_frame = 0;
        not_in_battle_count = 0;

        const auto& cur_opers = oper_analyzer.get_opers();
        size_t cooling = ranges::count_if(cur_opers, [](const auto& oper) { return oper.cooling; });

        if (oper_analyzer.get_in_detail_page()) {
            if (!in_segment || m_clips.empty()) {
#ifdef ASST_DEBUG
                pre_frame = frame;
#endif
                continue;
            }
            m_clips.back().end_frame = i - skip_count;
            in_segment = false;
#ifdef ASST_DEBUG
            pre_frame = frame;
#endif
            continue;
        }
        else if (!in_segment) {
            ClipInfo info;
            info.start_frame = i;
            info.end_frame = i;
            info.deployment = cur_opers;
            info.cooling = cooling;
            m_clips.emplace_back(std::move(info));

            in_segment = true;
        }
        else if (m_clips.back().deployment.size() != cur_opers.size()) {
            m_clips.back().end_frame = i;
            in_segment = false;
        }
        else if (cooling < m_clips.back().cooling) {
            // cooling 的干员识别率不如普通的，尽量识别 cooling 少的帧
            auto& backs = m_clips.back();
            backs.deployment = cur_opers;
            backs.cooling = cooling;
        }
#ifdef ASST_DEBUG
        pre_frame = frame;
#endif
    }

    callback(AsstMsg::SubTaskCompleted, basic_info_with_what("Slice"));
    return true;
}

bool asst::CombatRecordRecognitionTask::analyze_all_clips()
{
    LogTraceFunction;

    ClipInfo* pre_clip_ptr = nullptr;
    for (auto iter = m_clips.begin(); iter != m_clips.end();) {
        ClipInfo& clip = *iter;
        bool deployment_changed = false;
        if (pre_clip_ptr && clip.deployment.size() == pre_clip_ptr->deployment.size()) {
            for (size_t i = 0; i < clip.deployment.size(); ++i) {
                deployment_changed |= clip.deployment[i].role != pre_clip_ptr->deployment[i].role;
            }
        }
        else {
            deployment_changed = true;
        }

        if (!deployment_changed || clip.start_frame >= clip.end_frame) {
            Log.warn(__FUNCTION__, "deployment has no changes or frame error", clip.start_frame, clip.end_frame);
            iter = m_clips.erase(iter);
            continue;
        }
        analyze_clip(clip, pre_clip_ptr);
        pre_clip_ptr = &clip;
        ++iter;
    }
    return true;
}

bool asst::CombatRecordRecognitionTask::analyze_clip(ClipInfo& clip, ClipInfo* pre_clip_ptr)
{
    LogTraceFunction;

    if (!detect_operators(clip, pre_clip_ptr)) {
        return false;
    }
    if (!classify_direction(clip, pre_clip_ptr)) {
        return false;
    }
    if (!process_changes(clip, pre_clip_ptr)) {
        return false;
    }

    return true;
}

bool asst::CombatRecordRecognitionTask::detect_operators(ClipInfo& clip, [[maybe_unused]] ClipInfo* pre_clip_ptr)
{
    LogTraceFunction;

    callback(AsstMsg::SubTaskStart, basic_info_with_what("DetectOperators"));

    const size_t frame_count = clip.end_frame - clip.start_frame;

    /* detect operators on the battefield */
    using DetectionResult = std::unordered_set<Point>;
    std::unordered_map<DetectionResult, size_t, ContainerHasher<DetectionResult>> oper_det_samping;
    const Rect det_box_move = Task.get("BattleOperBoxRectMove")->rect_move;

    constexpr size_t OperDetSamplingCount = 5;
    const size_t skip_count = frame_count > (OperDetSamplingCount + 1) ? frame_count / (OperDetSamplingCount + 1) : 1;

    const size_t det_begin = clip.start_frame + skip_count;
    const size_t det_end = clip.end_frame - skip_count;
    m_video_ptr->set(cv::CAP_PROP_POS_FRAMES, static_cast<double>(det_begin));

    for (size_t i = det_begin; i < det_end; i += skip_frames(skip_count)) {
        cv::Mat frame;
        *m_video_ptr >> frame;
        if (frame.empty()) {
            Log.error(i, "frame is empty");
            callback(AsstMsg::SubTaskError, basic_info_with_what("DetectOperators"));
            return false;
        }

        cv::resize(frame, frame, cv::Size(), m_scale, m_scale, cv::INTER_AREA);
        BattleOperatorsImageAnalyzer analyzer(frame);
        analyzer.analyze();
        show_img(analyzer);

        DetectionResult cur_locations;
        auto tiles = m_normal_tile_info | views::values;
        for (const auto& box : analyzer.get_results()) {
            Rect rect = box.rect.move(det_box_move);
            auto iter = ranges::find_if(tiles, [&](const TilePack::TileInfo& t) { return rect.include(t.pos); });
            if (iter == tiles.end()) {
                Log.warn(i, __FUNCTION__, "no pos", box.rect.to_string(), rect);
                continue;
            }
            cur_locations.emplace((*iter).loc);
        }
        oper_det_samping[std::move(cur_locations)] += 1;
    }

    /* 取众数 */
    auto oper_det_iter = ranges::max_element(oper_det_samping,
                                             [&](const auto& lhs, const auto& rhs) { return lhs.second < rhs.second; });
    if (oper_det_iter == oper_det_samping.end()) {
        Log.error(__FUNCTION__, "oper_det_samping is empty");
        callback(AsstMsg::SubTaskError, basic_info_with_what("DetectOperators"));
        return false;
    }

    for (const Point& loc : oper_det_iter->first) {
        clip.battlefield.emplace(loc, BattlefiledOper {});
    }

    // if (pre_clip_ptr && clip.battlefield.size() < pre_clip_ptr->battlefield.size() &&
    //     clip.deployment.size() == pre_clip_ptr->deployment.size()) {
    //     // 战场上人少了；但部署区人没变。多半是 det 错了
    //     Log.warn(__FUNCTION__, "battlefield size less than pre_clip_ptr->battlefield.size()");
    //     for (const Point& loc : pre_clip_ptr->battlefield | views::keys) {
    //         clip.battlefield.emplace(loc, BattlefiledOper {});
    //     }
    // }

    callback(AsstMsg::SubTaskCompleted, basic_info_with_what("DetectOperators"));
    return true;
}

bool asst::CombatRecordRecognitionTask::classify_direction(ClipInfo& clip, ClipInfo* pre_clip_ptr)
{
    LogTraceFunction;

    if (!pre_clip_ptr) {
        Log.info("first clip, skip");
        callback(AsstMsg::SubTaskCompleted, basic_info_with_what("ClassifyDirection"));
        return true;
    }

    std::vector<Point> newcomer;
    for (const Point& loc : clip.battlefield | views::keys) {
        if (pre_clip_ptr->battlefield.contains(loc)) {
            continue;
        }
        newcomer.emplace_back(loc);
    }
    if (newcomer.empty()) {
        return true;
    }
    callback(AsstMsg::SubTaskStart, basic_info_with_what("ClassifyDirection"));

    const size_t frame_count = clip.end_frame - clip.start_frame;

    /* classify direction */
    constexpr size_t DirectionClsSamplingCount = 5;
    std::unordered_map<Point, std::unordered_map<battle::DeployDirection, size_t>> dir_cls_sampling;
    const size_t skip_count =
        frame_count > (DirectionClsSamplingCount + 1) ? frame_count / (DirectionClsSamplingCount + 1) : 1;

    const size_t dir_begin = clip.start_frame + skip_count;
    const size_t dir_end = clip.end_frame - skip_count;
    m_video_ptr->set(cv::CAP_PROP_POS_FRAMES, static_cast<double>(dir_begin));

    for (size_t i = dir_begin; i < dir_end; i += skip_frames(skip_count)) {
        cv::Mat frame;
        *m_video_ptr >> frame;
        if (frame.empty()) {
            Log.error(i, "frame is empty");
            callback(AsstMsg::SubTaskError, basic_info_with_what("ClassifyDirection"));
            return false;
        }

        cv::resize(frame, frame, cv::Size(), m_scale, m_scale, cv::INTER_AREA);
        BattleDeployDirectionImageAnalyzer analyzer(frame);
        for (const auto& loc : newcomer) {
            analyzer.set_base_point(m_normal_tile_info.at(loc).pos);
            analyzer.analyze();
            show_img(analyzer);
            auto dir = static_cast<battle::DeployDirection>(analyzer.get_class_id());
            dir_cls_sampling[loc][dir] += 1;
        }
    }

    /* 取众数 */
    for (const auto& [loc, sampling] : dir_cls_sampling) {
        auto dir_cls_iter =
            ranges::max_element(sampling, [&](const auto& lhs, const auto& rhs) { return lhs.second < rhs.second; });

        if (dir_cls_iter == sampling.end()) {
            Log.error(__FUNCTION__, "dir_cls_sampling is empty");
            callback(AsstMsg::SubTaskError, basic_info_with_what("ClassifyDirection"));
            return false;
        }
        clip.battlefield[loc].direction = dir_cls_iter->first;
        clip.battlefield[loc].new_here = true;
    }
    callback(AsstMsg::SubTaskCompleted, basic_info_with_what("ClassifyDirection"));
    return true;
}

bool asst::CombatRecordRecognitionTask::process_changes(ClipInfo& clip, ClipInfo* pre_clip_ptr)
{
    LogTraceFunction;
    std::ignore = m_video_ptr;
    std::ignore = clip;

    if (!pre_clip_ptr) {
        Log.info("first clip, skip");
        return true;
    }

    auto& actions_json = m_copilot_json["actions"].as_array();

    if (clip.deployment.size() == pre_clip_ptr->deployment.size()) {
        Log.warn("same deployment size", clip.deployment.size());
    }
    else if (clip.deployment.size() < pre_clip_ptr->deployment.size()) {
        // 部署
        std::vector<std::string> deployed;
        ananlyze_deployment_names(clip);
        ananlyze_deployment_names(*pre_clip_ptr);
        for (const auto& pre_oper : pre_clip_ptr->deployment) {
            auto iter = ranges::find_if(clip.deployment, [&](const auto& oper) { return oper.name == pre_oper.name; });
            if (iter != clip.deployment.end()) {
                continue;
            }
            deployed.emplace_back(pre_oper.name);
        }
        Log.info("deployed", deployed);

        if (deployed.empty()) {
            Log.warn("Unknown dployed");
            return false;
        }

        auto deployed_iter = deployed.begin();
        for (const auto& [loc, oper] : clip.battlefield) {
            if (!oper.new_here) {
                continue;
            }
            std::string name = deployed_iter == deployed.end() ? "UnkownDeployed" : *(deployed_iter++);
            json::object deploy_json {
                { "type", "Deploy" },
                { "name", name }, // 这里正常应该只有一个人，多了就只能抽奖了（
                { "location", json::array { loc.x, loc.y } },
                { "direction", static_cast<int>(oper.direction) },
            };
            Log.info("deploy json", deploy_json.to_string());
            actions_json.emplace_back(std::move(deploy_json));
        }
    }
    else {
        // 撤退
        for (const auto& [pre_loc, pre_oper] : pre_clip_ptr->battlefield) {
            if (clip.battlefield.contains(pre_loc)) {
                continue;
            }
            json::object retreat_json {
                { "type", "Retreat" },
                { "location", json::array { pre_loc.x, pre_loc.y } },
            };
            Log.info("retreat json", retreat_json.to_string());
            actions_json.emplace_back(std::move(retreat_json));
        }
    }

    return true;
}

void asst::CombatRecordRecognitionTask::ananlyze_deployment_names(ClipInfo& clip)
{
    LogTraceFunction;

    for (auto& oper : clip.deployment) {
        if (!oper.name.empty()) {
            continue;
        }
        BestMatchImageAnalyzer avatar_analyzer(oper.avatar);
        static const double threshold = Task.get<MatchTaskInfo>("BattleAvatarDataForVideo")->templ_threshold;
        avatar_analyzer.set_threshold(threshold);
        // static const double drone_threshold = Task.get<MatchTaskInfo>("BattleDroneAvatarData")->templ_threshold;
        // avatar_analyzer.set_threshold(oper.role == battle::Role::Drone ? drone_threshold : threshold);

        for (const auto& [name, avatar] : m_all_avatars) {
            std::unordered_set<battle::Role> roles = { BattleData.get_role(name) };
            if (name == "阿米娅") {
                roles.emplace(battle::Role::Warrior);
            }
            if (roles.contains(oper.role)) {
                avatar_analyzer.append_templ(name, avatar);
            }
        }
        bool analyzed = avatar_analyzer.analyze();
        // show_img(avatar_analyzer.get_draw());
        if (analyzed) {
            oper.name = avatar_analyzer.get_result().name;
        }
        else {
            oper.name = "UnknownDeployment";
        }
    }
}

size_t asst::CombatRecordRecognitionTask::skip_frames(size_t count)
{
    for (size_t i = 1; i < count; ++i) {
        cv::Mat ignore;
        *m_video_ptr >> ignore;
    }
    return count;
}

void asst::CombatRecordRecognitionTask::show_img(const asst::AbstractImageAnalyzer& analyzer)
{
#ifdef ASST_DEBUG
    show_img(analyzer.get_draw());
#else
    std::ignore = analyzer;
#endif
}

void asst::CombatRecordRecognitionTask::show_img(const cv::Mat& img)
{
#ifdef ASST_DEBUG
    cv::imshow(DrawWindow, img);
    cv::waitKey(1);
#else
    std::ignore = img;
#endif
}
