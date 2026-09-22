"""Tests for the /domains endpoint."""
import pytest

pytestmark = pytest.mark.asyncio


async def test_domains_returns_list(client):
    """GET /domains should return a list of domain objects."""
    response = await client.get("/domains")
    assert response.status_code == 200
    domains = response.json()
    assert isinstance(domains, list)
    assert len(domains) == 4


async def test_domains_have_required_fields(client):
    """Each domain should have id, name, icon, color, categories."""
    response = await client.get("/domains")
    domains = response.json()
    for domain in domains:
        assert "id" in domain
        assert "name" in domain
        assert "icon" in domain
        assert "color" in domain
        assert "categories" in domain
        assert isinstance(domain["categories"], list)


async def test_domains_contains_expected_ids(client):
    """The four expected domains should all be present."""
    response = await client.get("/domains")
    domains = response.json()
    ids = {d["id"] for d in domains}
    assert ids == {"sports", "music", "classroom", "general"}


async def test_domains_sports_has_correct_fields(client):
    """Sports domain should have the right name and categories."""
    response = await client.get("/domains")
    domains = {d["id"]: d for d in response.json()}
    sports = domains["sports"]
    assert sports["name"] == "Sports"
    assert "insight" in sports["categories"]
    assert "action" in sports["categories"]
    assert "event" in sports["categories"]


async def test_health_endpoint(client):
    """GET /health should return ok status."""
    response = await client.get("/health")
    assert response.status_code == 200
    body = response.json()
    assert body["status"] == "ok"
    assert body["mode"] == "mock"
