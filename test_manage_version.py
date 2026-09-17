def test_update_latest_symlink():
    import os
    from manage_version import update_latest_symlink
    with tempfile.TemporaryDirectory() as tmpdir:
        # Setup fake download structure
        base_dir = os.path.join(tmpdir, 'download')
        os.makedirs(base_dir)
        # Simulate two builds for develop
        build1 = os.path.join(base_dir, 'develop', 'build1', 'esp32', 'release', 'single_image')
        build2 = os.path.join(base_dir, 'develop', 'build2', 'esp32', 'release', 'single_image')
        os.makedirs(build1)
        os.makedirs(build2)
        # Create two app_merged.bin files
        with open(os.path.join(build1, 'app_merged.bin'), 'w') as f:
            f.write('old')
        with open(os.path.join(build2, 'app_merged.bin'), 'w') as f:
            f.write('new')
        # Prepare data with two entries, latest is build2
        data = {
            'firmware': [
                {
                    'soc': 'esp32',
                    'type': 'release',
                    'branch': 'develop',
                    'fw_version': 'V1.0-101',
                    'files': {'rom': {'url': f'file://{os.path.join(build1, "app_merged.bin")}'}}
                },
                {
                    'soc': 'esp32',
                    'type': 'release',
                    'branch': 'develop',
                    'fw_version': 'V1.0-102',
                    'files': {'rom': {'url': f'file://{os.path.join(build2, "app_merged.bin")}'}}
                }
            ],
            'history': []
        }
        update_latest_symlink(data, base_dir=base_dir)
        # Check that the symlink exists and points to the latest file
        latest_link = os.path.join(base_dir, 'latest-develop', 'app_merged.bin')
        assert os.path.islink(latest_link) or os.path.exists(latest_link)
        with open(latest_link, 'r') as f:
            content = f.read()
        assert content == 'new'
import os
import tempfile
import json
import shutil
import pytest
from manage_version import (
    add_or_update_entry,
    delete_entry,
    get_version_limit,
    list_entries,
    save_json,
    load_json,
    prime_json,
    cull_history
)

def test_add_and_list_entry():
    with tempfile.TemporaryDirectory() as tmpdir:
        json_path = os.path.join(tmpdir, 'test.json')
        data = prime_json(json_path)
        # Add entry
        data = add_or_update_entry(
            data,
            'esp32',
            'release',
            'develop',
            'V1.0.0-100-develop',
            'http://example.com/rom0.bin',
            'Release notes here',
        )
        save_json(data, json_path)
        # Reload and list
        data = load_json(json_path)
        entries = list_entries(data, 'esp32', 'release', 'develop')
        assert len(entries) == 1
        assert entries[0]['fw_version'] == 'V1.0.0-100-develop'
        assert entries[0]['files']['rom']['url'] == 'http://example.com/rom0.bin'
        assert entries[0]['comment'] == 'Release notes here'

def test_delete_entry():
    with tempfile.TemporaryDirectory() as tmpdir:
        json_path = os.path.join(tmpdir, 'test.json')
        data = prime_json(json_path)
        data = add_or_update_entry(data, 'esp32', 'release', 'develop', 'V1.0.0-100-develop', 'http://example.com/rom0.bin')
        save_json(data, json_path)
        # Delete entry
        data = delete_entry(data, 'esp32', 'release', 'develop', 'V1.0.0-100-develop', delete_files=False)
        save_json(data, json_path)
        data = load_json(json_path)
        entries = list_entries(data, 'esp32', 'release', 'develop')
        assert len(entries) == 0

def test_version_limit():
    assert get_version_limit('stable') == 4
    assert get_version_limit('testing') == 4
    assert get_version_limit('develop') == 10
    assert get_version_limit('foobar') == 10

def test_cull_history():
    with tempfile.TemporaryDirectory() as tmpdir:
        json_path = os.path.join(tmpdir, 'test.json')
        data = prime_json(json_path)
        # Add 12 versions to develop
        for i in range(12):
            v = f'V1.0.0-{100+i}-develop'
            data = add_or_update_entry(data, 'esp32', 'release', 'develop', v, f'http://example.com/rom{i}.bin')
        save_json(data, json_path)
        # Cull history
        data = cull_history(data, dry_run=False)
        save_json(data, json_path)
        data = load_json(json_path)
        entries = list_entries(data, 'esp32', 'release', 'develop', include_history=True)
        # Should only keep 10 versions for develop
        kept = [e for e in entries if not e.get('is_history', False)]
        assert len(kept) <= 10

def test_add_same_version_preserves_existing_comment_when_new_comment_missing():
    with tempfile.TemporaryDirectory() as tmpdir:
        json_path = os.path.join(tmpdir, 'test.json')
        data = prime_json(json_path)
        data = add_or_update_entry(
            data,
            'esp32',
            'release',
            'develop',
            'V1.0.0-100-develop',
            'http://example.com/rom0.bin',
            'Original release notes',
        )
        data = add_or_update_entry(
            data,
            'esp32',
            'release',
            'develop',
            'V1.0.0-100-develop',
            'http://example.com/rom1.bin',
            '',
        )
        assert data['firmware'][0]['comment'] == 'Original release notes'
        assert data['firmware'][0]['files']['rom']['url'] == 'http://example.com/rom1.bin'
